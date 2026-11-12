#include "assetDatabase.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <httplib.h>
#include <nlohmann/json.hpp>

#define __TBB_NO_IMPLICIT_LINKAGE 1
#pragma warning(push, 0)
#pragma warning(disable: 4244 4273 4305 4996)
#include <oneapi/tbb/concurrent_vector.h>
#include <oneapi/tbb/task_group.h>
#include <pxr/pxr.h>
#include <pxr/base/plug/registry.h>
#include <pxr/usd/sdf/fileFormat.h>
#pragma warning(pop)

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <shared_mutex>
#include <thread>
#include <unordered_set>
#include <utility>

namespace {

std::string PathToUtf8(const std::filesystem::path& path)
{
#ifdef _WIN32
    const std::wstring& wide = path.native();
    if (wide.empty())
        return {};
    const int utf8Len = WideCharToMultiByte(
        CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    if (utf8Len <= 0)
        return {};
    std::string utf8(static_cast<size_t>(utf8Len), '\0');
    WideCharToMultiByte(
        CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), utf8.data(), utf8Len, nullptr, nullptr);
    return utf8;
#else
    return path.string();
#endif
}

std::filesystem::path PathFromUtf8(std::string_view utf8)
{
#ifdef _WIN32
    if (utf8.empty())
        return {};
    const int wideLen = MultiByteToWideChar(
        CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (wideLen <= 0)
        return {};
    std::wstring wide(static_cast<size_t>(wideLen), L'\0');
    MultiByteToWideChar(
        CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), wide.data(), wideLen);
    return std::filesystem::path(wide);
#else
    return std::filesystem::path(utf8);
#endif
}

std::string ToLowerAscii(std::string_view input)
{
    std::string result(input.size(), '\0');
    std::transform(input.begin(), input.end(), result.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return result;
}

#ifdef _WIN32
std::filesystem::path GetModuleDirectory(HMODULE moduleHandle)
{
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
        const DWORD len = GetModuleFileNameW(moduleHandle, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (len == 0)
            return {};
        if (len < buffer.size() - 1) {
            buffer.resize(len);
            return std::filesystem::path(buffer).parent_path();
        }
        buffer.resize(buffer.size() * 2);
    }
}

std::filesystem::path GetCurrentDllDirectory()
{
    HMODULE thisModule = nullptr;
    if (!GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&GetCurrentDllDirectory),
            &thisModule)) {
        return {};
    }
    return GetModuleDirectory(thisModule);
}
#endif

void RegisterUsdPluginsForDirectory(const std::filesystem::path& baseDir, std::vector<std::string>& pluginPaths)
{
    if (baseDir.empty())
        return;

    auto addIfExists = [&pluginPaths](const std::filesystem::path& p) {
        std::error_code ec;
        if (std::filesystem::exists(p, ec))
            pluginPaths.push_back(PathToUtf8(p));
    };

    addIfExists(baseDir / "usd" / "plugInfo.json");
    addIfExists(baseDir / "plugInfo.json");
    addIfExists(baseDir / "resources" / "plugInfo.json");

    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(baseDir, ec)) {
        if (!entry.is_directory(ec))
            continue;
        if (entry.path().filename() == "usd")
            continue;
        addIfExists(entry.path() / "plugInfo.json");
        addIfExists(entry.path() / "resources" / "plugInfo.json");
    }

    if (!std::filesystem::exists(baseDir / "usd" / "plugInfo.json", ec)) {
        const std::filesystem::path workspaceSdkRoot = baseDir.parent_path().parent_path() / "externals" / "usd";
        addIfExists(workspaceSdkRoot / "lib" / "usd" / "plugInfo.json");
        addIfExists(workspaceSdkRoot / "plugin" / "usd" / "plugInfo.json");
    }
}

void EnsureUsdPluginsRegistered()
{
    static std::once_flag onceFlag;
    std::call_once(onceFlag, []() {
        std::vector<std::string> pluginPaths;
#ifdef _WIN32
        const std::filesystem::path dllDir = GetCurrentDllDirectory();
        const std::filesystem::path exeDir = GetModuleDirectory(nullptr);
        RegisterUsdPluginsForDirectory(dllDir, pluginPaths);
        if (!exeDir.empty() && exeDir != dllDir)
            RegisterUsdPluginsForDirectory(exeDir, pluginPaths);
#endif
        if (!pluginPaths.empty())
            pxr::PlugRegistry::GetInstance().RegisterPlugins(pluginPaths);
    });
}

std::unordered_set<std::string> CollectSupportedExtensions()
{
    EnsureUsdPluginsRegistered();

    std::unordered_set<std::string> extensions = {
        "fbx",
        "usd",
        "usda",
        "usdc",
        "usdz",
    };

    const std::set<std::string> sdfExtensions = pxr::SdfFileFormat::FindAllFileFormatExtensions();
    for (const std::string& ext : sdfExtensions) {
        std::string_view view = ext;
        while (!view.empty() && view.front() == '.')
            view.remove_prefix(1);
        if (!view.empty())
            extensions.insert(ToLowerAscii(view));
    }

    return extensions;
}

bool IsSupportedAssetFile(
    const std::filesystem::path& filePath,
    const std::string& fileNameUtf8,
    const std::unordered_set<std::string>& supportedExtensions)
{
    const std::string extFromPath = PathToUtf8(filePath.extension());
    if (!extFromPath.empty()) {
        std::string_view view = extFromPath;
        while (!view.empty() && view.front() == '.')
            view.remove_prefix(1);
        if (!view.empty() && supportedExtensions.contains(ToLowerAscii(view)))
            return true;
    }

    const std::string sdfExt = pxr::SdfFileFormat::GetFileExtension(fileNameUtf8);
    if (!sdfExt.empty() && supportedExtensions.contains(ToLowerAscii(sdfExt)))
        return true;

    return false;
}

std::optional<std::string> NormalizeRelativeDir(std::string_view rawDir)
{
    std::string normalized;
    size_t pos = 0;
    while (pos < rawDir.size()) {
        while (pos < rawDir.size() && (rawDir[pos] == '/' || rawDir[pos] == '\\'))
            ++pos;
        if (pos >= rawDir.size())
            break;

        size_t end = pos;
        while (end < rawDir.size() && rawDir[end] != '/' && rawDir[end] != '\\')
            ++end;

        const std::string_view segment = rawDir.substr(pos, end - pos);
        if (segment == "..")
            return std::nullopt;

        if (!segment.empty() && segment != ".") {
            if (!normalized.empty())
                normalized.push_back('/');
            normalized.append(segment);
        }

        pos = end;
    }

    return normalized;
}

std::wstring MakeNormalizedPathKey(const std::filesystem::path& path)
{
    std::error_code ec;
    std::filesystem::path norm = std::filesystem::weakly_canonical(path, ec);
    if (ec)
        norm = path.lexically_normal();

#ifdef _WIN32
    std::wstring wide = norm.native();
    for (wchar_t& ch : wide) {
        if (ch == L'/')
            ch = L'\\';
    }
    if (!wide.empty())
        ::CharLowerBuffW(wide.data(), static_cast<DWORD>(wide.size()));
    return wide;
#else
    return norm.wstring();
#endif
}

bool IsScreenshotNewerThanSource(
    const std::filesystem::path& sourcePath,
    const std::filesystem::path& screenshotPath)
{
    std::error_code ec;
    if (!std::filesystem::is_regular_file(sourcePath, ec) || ec)
        return true;
    const auto sourceWriteTime = std::filesystem::last_write_time(sourcePath, ec);
    if (ec)
        return false;

    if (!std::filesystem::is_regular_file(screenshotPath, ec) || ec)
        return false;
    if (std::filesystem::file_size(screenshotPath, ec) == 0 || ec)
        return false;
    const auto screenshotWriteTime = std::filesystem::last_write_time(screenshotPath, ec);
    if (ec)
        return false;

    return screenshotWriteTime > sourceWriteTime;
}

#ifdef _WIN32
constexpr std::string_view kWorkerPrefixReady = "@@READY";
constexpr std::string_view kWorkerPrefixOk = "@@OK";
constexpr std::string_view kWorkerPrefixErr = "@@ERR";
constexpr std::string_view kWorkerCommandQuit = "@@QUIT";

std::string SanitizeSingleLine(std::string text)
{
    for (char& ch : text) {
        if (ch == '\r' || ch == '\n' || ch == '\t')
            ch = ' ';
    }
    return text;
}

std::wstring QuoteWin32Arg(std::wstring_view arg)
{
    if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring_view::npos)
        return std::wstring(arg);

    std::wstring quoted = L"\"";
    for (auto it = arg.begin();; ++it) {
        size_t backslashes = 0;
        while (it != arg.end() && *it == L'\\') {
            ++it;
            ++backslashes;
        }
        if (it == arg.end()) {
            quoted.append(backslashes * 2, L'\\');
            break;
        }
        if (*it == L'"') {
            quoted.append(backslashes * 2 + 1, L'\\');
            quoted.push_back(L'"');
        }
        else {
            quoted.append(backslashes, L'\\');
            quoted.push_back(*it);
        }
    }
    quoted.push_back(L'"');
    return quoted;
}

std::filesystem::path ResolveScreenshotExecutablePath()
{
    const std::filesystem::path dllDir = GetCurrentDllDirectory();
    if (!dllDir.empty()) {
        const std::filesystem::path candidate = dllDir / L"usdScreenshot.exe";
        std::error_code ec;
        if (std::filesystem::is_regular_file(candidate, ec) && !ec)
            return candidate;
    }

    const std::filesystem::path exeDir = GetModuleDirectory(nullptr);
    if (!exeDir.empty() && exeDir != dllDir) {
        const std::filesystem::path candidate = exeDir / L"usdScreenshot.exe";
        std::error_code ec;
        if (std::filesystem::is_regular_file(candidate, ec) && !ec)
            return candidate;
    }

    return {};
}

class ScopedJobObject
{
public:
    ScopedJobObject()
    {
        m_job = ::CreateJobObjectW(nullptr, nullptr);
        if (m_job) {
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
            info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            ::SetInformationJobObject(m_job, JobObjectExtendedLimitInformation, &info, sizeof(info));
        }
    }

    ~ScopedJobObject()
    {
        if (m_job)
            ::CloseHandle(m_job);
    }

    ScopedJobObject(const ScopedJobObject&) = delete;
    ScopedJobObject& operator=(const ScopedJobObject&) = delete;

    void Assign(HANDLE process) const
    {
        if (m_job && process)
            ::AssignProcessToJobObject(m_job, process);
    }

private:
    HANDLE m_job = nullptr;
};

class ScreenshotWorkerClient
{
public:
    ScreenshotWorkerClient() = default;

    ~ScreenshotWorkerClient()
    {
        Shutdown(true);
    }

    ScreenshotWorkerClient(const ScreenshotWorkerClient&) = delete;
    ScreenshotWorkerClient& operator=(const ScreenshotWorkerClient&) = delete;

    bool Start(
        const std::wstring& commandLine,
        const std::filesystem::path& workingDirectory,
        const ScopedJobObject& jobObject,
        std::mutex& spawnMutex)
    {
        Shutdown(true);

        HANDLE stdinRead = nullptr;
        HANDLE stdinWrite = nullptr;
        HANDLE stdoutRead = nullptr;
        HANDLE stdoutWrite = nullptr;
        HANDLE stderrHandle = nullptr;
        bool closeStderrHandle = false;

        {
            std::lock_guard<std::mutex> lock(spawnMutex);

            SECURITY_ATTRIBUTES saInherit{};
            saInherit.nLength = sizeof(saInherit);
            saInherit.bInheritHandle = TRUE;

            if (!::CreatePipe(&stdinRead, &stdinWrite, &saInherit, 0))
                return false;
            ::SetHandleInformation(stdinWrite, HANDLE_FLAG_INHERIT, 0);

            if (!::CreatePipe(&stdoutRead, &stdoutWrite, &saInherit, 0)) {
                ::CloseHandle(stdinRead);
                ::CloseHandle(stdinWrite);
                return false;
            }
            ::SetHandleInformation(stdoutRead, HANDLE_FLAG_INHERIT, 0);

            stderrHandle = ::CreateFileW(
                L"NUL",
                GENERIC_WRITE,
                FILE_SHARE_READ | FILE_SHARE_WRITE,
                &saInherit,
                OPEN_EXISTING,
                FILE_ATTRIBUTE_NORMAL,
                nullptr);
            if (stderrHandle != INVALID_HANDLE_VALUE)
                closeStderrHandle = true;
            else
                stderrHandle = nullptr;

            HANDLE inheritedHandles[3] = { stdinRead, stdoutWrite, stderrHandle };
            DWORD inheritedCount = stderrHandle ? 3 : 2;
            if (inheritedCount == 3 &&
                (inheritedHandles[2] == inheritedHandles[0] || inheritedHandles[2] == inheritedHandles[1])) {
                inheritedCount = 2;
            }

            SIZE_T attrListSize = 0;
            ::InitializeProcThreadAttributeList(nullptr, 1, 0, &attrListSize);
            std::vector<unsigned char> attrListBuffer(attrListSize);
            auto* attrList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attrListBuffer.data());

            if (!::InitializeProcThreadAttributeList(attrList, 1, 0, &attrListSize) ||
                !::UpdateProcThreadAttribute(
                    attrList,
                    0,
                    PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                    inheritedHandles,
                    inheritedCount * sizeof(HANDLE),
                    nullptr,
                    nullptr)) {
                if (attrList)
                    ::DeleteProcThreadAttributeList(attrList);
                ::CloseHandle(stdinRead);
                ::CloseHandle(stdinWrite);
                ::CloseHandle(stdoutRead);
                ::CloseHandle(stdoutWrite);
                if (closeStderrHandle && stderrHandle)
                    ::CloseHandle(stderrHandle);
                return false;
            }

            STARTUPINFOEXW si{};
            si.StartupInfo.cb = sizeof(si);
            si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
            si.StartupInfo.hStdInput = stdinRead;
            si.StartupInfo.hStdOutput = stdoutWrite;
            si.StartupInfo.hStdError = stderrHandle;
            si.lpAttributeList = attrList;

            std::wstring mutableCmd = commandLine;
            const LPCWSTR workDirPtr = workingDirectory.empty() ? nullptr : workingDirectory.c_str();
            PROCESS_INFORMATION pi{};
            const BOOL created = ::CreateProcessW(
                nullptr,
                mutableCmd.data(),
                nullptr,
                nullptr,
                TRUE,
                EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW,
                nullptr,
                workDirPtr,
                &si.StartupInfo,
                &pi);

            ::DeleteProcThreadAttributeList(attrList);
            ::CloseHandle(stdinRead);
            ::CloseHandle(stdoutWrite);
            if (closeStderrHandle && stderrHandle)
                ::CloseHandle(stderrHandle);

            if (!created) {
                ::CloseHandle(stdinWrite);
                ::CloseHandle(stdoutRead);
                return false;
            }

            jobObject.Assign(pi.hProcess);
            ::CloseHandle(pi.hThread);

            {
                std::lock_guard<std::mutex> procLock(m_procMutex);
                m_process = pi.hProcess;
            }
            m_stdinWrite = stdinWrite;
            m_stdoutRead = stdoutRead;
            m_readBuffer.clear();
        }

        std::string line;
        while (ReadLine(&line)) {
            if (line == kWorkerPrefixReady)
                return true;
            if (line.starts_with(kWorkerPrefixErr)) {
                Shutdown(true);
                return false;
            }
        }

        Shutdown(true);
        return false;
    }

    bool ExecuteTask(
        const std::string& sourcePathUtf8,
        const std::string& outputPathUtf8,
        bool* outWorkerCrashed)
    {
        if (outWorkerCrashed)
            *outWorkerCrashed = false;

        std::string payload = SanitizeSingleLine(sourcePathUtf8);
        payload.push_back('\t');
        payload += SanitizeSingleLine(outputPathUtf8);
        payload.push_back('\n');

        if (!WriteAll(payload)) {
            if (outWorkerCrashed)
                *outWorkerCrashed = true;
            return false;
        }

        std::string line;
        while (ReadLine(&line)) {
            if (line == kWorkerPrefixOk)
                return true;
            if (line.starts_with(kWorkerPrefixErr))
                return false;
        }

        if (outWorkerCrashed)
            *outWorkerCrashed = true;
        return false;
    }

    bool IsRunning() const
    {
        std::lock_guard<std::mutex> procLock(m_procMutex);
        return m_process != nullptr && m_stdinWrite != nullptr && m_stdoutRead != nullptr;
    }

    void TerminateChildProcess()
    {
        std::lock_guard<std::mutex> procLock(m_procMutex);
        if (m_process)
            ::TerminateProcess(m_process, 1);
    }

    void Shutdown(bool forceTerminateIfAlive)
    {
        if (m_stdinWrite) {
            if (!forceTerminateIfAlive) {
                const std::string quitCmd = std::string(kWorkerCommandQuit) + "\n";
                WriteAll(quitCmd);
            }
            ::CloseHandle(m_stdinWrite);
            m_stdinWrite = nullptr;
        }

        if (m_stdoutRead) {
            ::CloseHandle(m_stdoutRead);
            m_stdoutRead = nullptr;
        }

        HANDLE processToClose = nullptr;
        {
            std::lock_guard<std::mutex> procLock(m_procMutex);
            processToClose = m_process;
            m_process = nullptr;
        }

        if (processToClose) {
            const DWORD waitMs = forceTerminateIfAlive ? 100 : 2000;
            if (::WaitForSingleObject(processToClose, waitMs) != WAIT_OBJECT_0)
                ::TerminateProcess(processToClose, 1);
            ::CloseHandle(processToClose);
        }

        m_readBuffer.clear();
    }

private:
    bool WriteAll(std::string_view data) const
    {
        if (!m_stdinWrite)
            return false;
        size_t offset = 0;
        while (offset < data.size()) {
            const DWORD chunk = static_cast<DWORD>(std::min<size_t>(data.size() - offset, 64 * 1024));
            DWORD written = 0;
            if (!::WriteFile(m_stdinWrite, data.data() + offset, chunk, &written, nullptr) || written == 0)
                return false;
            offset += written;
        }
        return true;
    }

    bool ReadLine(std::string* outLine)
    {
        if (!m_stdoutRead)
            return false;

        for (;;) {
            const size_t newlinePos = m_readBuffer.find('\n');
            if (newlinePos != std::string::npos) {
                std::string line = m_readBuffer.substr(0, newlinePos);
                m_readBuffer.erase(0, newlinePos + 1);
                if (!line.empty() && line.back() == '\r')
                    line.pop_back();
                *outLine = std::move(line);
                return true;
            }

            char chunk[4096];
            DWORD bytesRead = 0;
            if (!::ReadFile(m_stdoutRead, chunk, sizeof(chunk), &bytesRead, nullptr) || bytesRead == 0) {
                if (!m_readBuffer.empty()) {
                    std::string line = std::move(m_readBuffer);
                    m_readBuffer.clear();
                    if (!line.empty() && line.back() == '\r')
                        line.pop_back();
                    *outLine = std::move(line);
                    return true;
                }
                return false;
            }
            m_readBuffer.append(chunk, bytesRead);
        }
    }

    mutable std::mutex m_procMutex;
    HANDLE m_process = nullptr;
    HANDLE m_stdinWrite = nullptr;
    HANDLE m_stdoutRead = nullptr;
    std::string m_readBuffer;
};
#endif // _WIN32

class ScreenshotWorkerPool
{
public:
    static constexpr size_t kWorkerCount = 4;
    static constexpr size_t kImageWidth = 512;

    ScreenshotWorkerPool() = default;

    ~ScreenshotWorkerPool() noexcept
    {
        Stop();
    }

    ScreenshotWorkerPool(const ScreenshotWorkerPool&) = delete;
    ScreenshotWorkerPool& operator=(const ScreenshotWorkerPool&) = delete;

    void Start()
    {
#ifdef _WIN32
        std::lock_guard<std::mutex> lifecycleLock(m_lifecycleMutex);

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_started)
                return;

            m_stopping = false;
            m_workerExePath = ResolveScreenshotExecutablePath();
            m_workerWorkDir = m_workerExePath.empty() ? std::filesystem::path() : m_workerExePath.parent_path();

            if (!m_workerExePath.empty()) {
                const unsigned int hwThreads = std::max(1u, std::thread::hardware_concurrency());
                const unsigned int threadsPerWorker =
                    std::max(1u, hwThreads / static_cast<unsigned int>(kWorkerCount));
                m_workerCommandLine = QuoteWin32Arg(m_workerExePath.native()) +
                                      L" --worker --imageWidth " + std::to_wstring(kImageWidth) +
                                      L" --threadsPerWorker " + std::to_wstring(threadsPerWorker);
            }
            else {
                m_workerCommandLine.clear();
            }

            for (size_t i = 0; i < kWorkerCount; ++i) {
                m_slots[i].disabled = m_workerCommandLine.empty();
                m_slots[i].busy.store(false);
                m_slots[i].scheduled = false;
            }
        }

        if (!m_workerCommandLine.empty()) {
            for (size_t i = 0; i < kWorkerCount; ++i)
                EnsureWorkerProcessStarted(m_slots[i]);
        }

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_started = true;
        }
#endif
    }

    void Stop()
    {
#ifdef _WIN32
        std::lock_guard<std::mutex> lifecycleLock(m_lifecycleMutex);

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (!m_started)
                return;

            m_stopping = true;
            for (const Task& pending : m_queue)
                m_activeKeys.erase(pending.key);
            m_queue.clear();
        }

        m_completionCv.notify_all();

        for (size_t i = 0; i < kWorkerCount; ++i) {
            if (m_slots[i].busy.load())
                m_slots[i].client.TerminateChildProcess();
        }

        m_taskGroup.wait();

        for (size_t i = 0; i < kWorkerCount; ++i)
            m_slots[i].client.Shutdown(false);

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_activeKeys.clear();
            for (size_t i = 0; i < kWorkerCount; ++i)
                m_slots[i].scheduled = false;
            m_started = false;
            m_stopping = false;
        }
        m_completionCv.notify_all();
#endif
    }

    void EnqueueDirectoryFiles(
        const std::filesystem::path& directoryPath,
        const std::vector<std::string>& fileNames)
    {
#ifdef _WIN32
        if (fileNames.empty())
            return;

        Start();

        std::vector<Task> candidates;
        candidates.reserve(fileNames.size());

        for (const std::string& fileName : fileNames) {
            const std::filesystem::path srcPath = directoryPath / PathFromUtf8(fileName);
            std::filesystem::path jpgPath = srcPath;
            jpgPath += L".jpg";

            if (IsScreenshotNewerThanSource(srcPath, jpgPath))
                continue;

            candidates.push_back(Task{
                srcPath,
                jpgPath,
                MakeNormalizedPathKey(jpgPath),
            });
        }

        if (candidates.empty())
            return;

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (!m_started || m_stopping)
                return;

            bool anyWorkerUsable = false;
            for (size_t i = 0; i < kWorkerCount; ++i) {
                if (!m_slots[i].disabled) {
                    anyWorkerUsable = true;
                    break;
                }
            }
            if (!anyWorkerUsable)
                return;

            for (Task& task : candidates) {
                if (m_activeKeys.contains(task.key))
                    continue;
                m_activeKeys.insert(task.key);
                m_queue.push_back(std::move(task));
            }

            size_t pendingTasks = m_queue.size();
            for (size_t i = 0; i < kWorkerCount && pendingTasks > 0; ++i) {
                if (!m_slots[i].disabled && !m_slots[i].scheduled) {
                    m_slots[i].scheduled = true;
                    --pendingTasks;
                    m_taskGroup.run([this, i]() {
                        DrainQueueWithWorker(i);
                    });
                }
            }
        }
#else
        (void)directoryPath;
        (void)fileNames;
#endif
    }

    void WaitForPendingScreenshot(const std::filesystem::path& screenshotPath)
    {
#ifdef _WIN32
        const std::wstring key = MakeNormalizedPathKey(screenshotPath);
        std::unique_lock<std::mutex> lock(m_mutex);
        if (!m_activeKeys.contains(key))
            return;

        for (auto it = m_queue.begin(); it != m_queue.end(); ++it) {
            if (it->key == key) {
                if (it != m_queue.begin()) {
                    Task promoted = std::move(*it);
                    m_queue.erase(it);
                    m_queue.push_front(std::move(promoted));
                }
                break;
            }
        }

        m_completionCv.wait(lock, [this, &key]() {
            return m_stopping || !m_activeKeys.contains(key);
        });
#else
        (void)screenshotPath;
#endif
    }

private:
#ifdef _WIN32
    struct Task
    {
        std::filesystem::path sourcePath;
        std::filesystem::path screenshotPath;
        std::wstring key;
    };

    struct WorkerSlot
    {
        ScreenshotWorkerClient client;
        std::atomic<bool> busy{ false };
        bool scheduled = false;
        bool disabled = false;
    };

    void EnsureWorkerProcessStarted(WorkerSlot& slot)
    {
        if (slot.disabled || slot.client.IsRunning() || m_stopping)
            return;

        slot.busy.store(true);
        const bool started = slot.client.Start(
            m_workerCommandLine, m_workerWorkDir, m_jobObject, m_spawnMutex);
        slot.busy.store(false);

        if (!started && !m_stopping)
            slot.disabled = true;
    }

    void DrainQueueWithWorker(size_t workerIndex)
    {
        WorkerSlot& slot = m_slots[workerIndex];

        for (;;) {
            Task task;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                if (m_stopping || m_queue.empty()) {
                    slot.scheduled = false;
                    return;
                }

                task = std::move(m_queue.front());
                m_queue.pop_front();
            }

            ExecuteScreenshotTask(workerIndex, slot, task);

            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_activeKeys.erase(task.key);
            }
            m_completionCv.notify_all();
        }
    }

    void ExecuteScreenshotTask(size_t workerIndex, WorkerSlot& slot, const Task& task)
    {
        if (m_stopping || IsScreenshotNewerThanSource(task.sourcePath, task.screenshotPath))
            return;

        EnsureWorkerProcessStarted(slot);
        if (!slot.client.IsRunning())
            return;

        std::filesystem::path tempPath = task.screenshotPath;
        tempPath += L".tmp." + std::to_wstring(::GetCurrentProcessId()) + L"_" +
                    std::to_wstring(workerIndex) + L".jpg";

        bool workerCrashed = false;
        slot.busy.store(true);
        const bool ok = slot.client.ExecuteTask(
            PathToUtf8(task.sourcePath),
            PathToUtf8(tempPath),
            &workerCrashed);
        slot.busy.store(false);

        if (ok) {
            if (!::MoveFileExW(
                    tempPath.c_str(),
                    task.screenshotPath.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
                ::Sleep(15);
                if (!::MoveFileExW(
                        tempPath.c_str(),
                        task.screenshotPath.c_str(),
                        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
                    ::DeleteFileW(tempPath.c_str());
                }
            }
        }
        else {
            ::DeleteFileW(tempPath.c_str());
            if (workerCrashed)
                slot.client.Shutdown(true);
        }
    }

    std::mutex m_lifecycleMutex;
    std::mutex m_mutex;
    std::condition_variable m_completionCv;
    std::deque<Task> m_queue;
    std::unordered_set<std::wstring> m_activeKeys;
    bool m_started = false;
    std::atomic<bool> m_stopping{ false };

    tbb::task_group m_taskGroup;
    ScopedJobObject m_jobObject;
    std::mutex m_spawnMutex;
    std::filesystem::path m_workerExePath;
    std::filesystem::path m_workerWorkDir;
    std::wstring m_workerCommandLine;
    std::array<WorkerSlot, kWorkerCount> m_slots;
#endif
};

class AssetDatabase final : public IAssetDatabase
{
public:
    AssetDatabase() = default;

    ~AssetDatabase() noexcept override
    {
        StopServer();
        m_scanTaskGroup.wait();
    }

    AssetDatabase(const AssetDatabase&) = delete;
    AssetDatabase& operator=(const AssetDatabase&) = delete;

    void Release() override
    {
        delete this;
    }

    bool Scan(const std::filesystem::path& rootDir) override
    {
        std::error_code ec;
        const std::filesystem::path normalizedRoot = std::filesystem::weakly_canonical(rootDir, ec);
        const std::filesystem::path effectiveRoot = ec ? rootDir.lexically_normal() : normalizedRoot;

        ec.clear();
        if (effectiveRoot.empty() || !std::filesystem::exists(effectiveRoot, ec) ||
            !std::filesystem::is_directory(effectiveRoot, ec)) {
            return false;
        }

        const std::unordered_set<std::string> supportedExtensions = CollectSupportedExtensions();
        tbb::concurrent_vector<std::pair<std::string, std::vector<std::string>>> matchedDirFiles;

        {
            std::lock_guard<std::mutex> scanLock(m_scanMutex);
            m_scanTaskGroup.run([this, &effectiveRoot, &supportedExtensions, &matchedDirFiles]() {
                ScanDirectoryTask(effectiveRoot, "", supportedExtensions, matchedDirFiles);
            });
            m_scanTaskGroup.wait();
        }

        std::map<std::string, DirectoryContents> scannedDirectories;
        for (auto& [relDir, files] : matchedDirFiles) {
            scannedDirectories[relDir].files = std::move(files);

            std::string_view current = relDir;
            while (!current.empty()) {
                const size_t slashPos = current.rfind('/');
                const std::string parentDir =
                    (slashPos == std::string_view::npos) ? std::string() : std::string(current.substr(0, slashPos));
                const std::string childName =
                    (slashPos == std::string_view::npos) ? std::string(current) : std::string(current.substr(slashPos + 1));

                scannedDirectories[parentDir].directories.push_back(std::move(childName));
                if (slashPos == std::string_view::npos)
                    break;
                current = current.substr(0, slashPos);
            }
        }

        for (auto& [_, contents] : scannedDirectories) {
            std::sort(contents.directories.begin(), contents.directories.end());
            contents.directories.erase(
                std::unique(contents.directories.begin(), contents.directories.end()),
                contents.directories.end());
        }

        std::unique_lock lock(m_dataMutex);
        m_rootDirectory = effectiveRoot;
        m_directories = std::move(scannedDirectories);
        return true;
    }

    void Clear() override
    {
        std::unique_lock lock(m_dataMutex);
        m_rootDirectory.clear();
        m_directories.clear();
    }

    std::filesystem::path GetRootDirectory() const override
    {
        std::shared_lock lock(m_dataMutex);
        return m_rootDirectory;
    }

    std::vector<std::string> GetDirectories() const override
    {
        std::shared_lock lock(m_dataMutex);
        std::vector<std::string> result;
        result.reserve(m_directories.size());
        for (const auto& [dirPath, _] : m_directories)
            result.push_back(dirPath);
        return result;
    }

    std::optional<DirectoryContents> ListDirectory(std::string_view relativeDir) const override
    {
        const std::optional<std::string> normalized = NormalizeRelativeDir(relativeDir);
        if (!normalized.has_value())
            return std::nullopt;

        std::shared_lock lock(m_dataMutex);
        const auto it = m_directories.find(*normalized);
        if (it == m_directories.end())
            return std::nullopt;
        return it->second;
    }

    bool StartServer(const std::string& host, int port) override
    {
        std::lock_guard lock(m_serverMutex);

        std::unique_ptr<httplib::Server> svr;
        if (!BindServerLocked(host, port, svr))
            return false;

        m_screenshotPool.Start();

        m_server = std::move(svr);
        httplib::Server* rawServer = m_server.get();
        m_running.store(true);

        m_serverThread = std::thread([this, rawServer]() {
            rawServer->listen_after_bind();
            m_running.store(false);
        });

        return true;
    }

    bool RunServer(const std::string& host, int port) override
    {
        httplib::Server* rawServer = nullptr;
        {
            std::lock_guard lock(m_serverMutex);
            std::unique_ptr<httplib::Server> svr;
            if (!BindServerLocked(host, port, svr))
                return false;

            m_screenshotPool.Start();

            m_server = std::move(svr);
            rawServer = m_server.get();
            m_running.store(true);
        }

        const bool ok = rawServer->listen_after_bind();

        m_screenshotPool.Stop();

        {
            std::lock_guard lock(m_serverMutex);
            m_running.store(false);
            m_boundPort.store(0);
            m_server.reset();
        }
        return ok;
    }

    void StopServer() override
    {
        std::thread threadToJoin;
        {
            std::lock_guard lock(m_serverMutex);
            if (m_server)
                m_server->stop();
            if (m_serverThread.joinable() && m_serverThread.get_id() != std::this_thread::get_id())
                threadToJoin = std::move(m_serverThread);
        }

        m_screenshotPool.Stop();

        if (threadToJoin.joinable())
            threadToJoin.join();

        {
            std::lock_guard lock(m_serverMutex);
            m_server.reset();
            m_boundPort.store(0);
            m_running.store(false);
        }
    }

    bool IsServerRunning() const override
    {
        return m_running.load();
    }

    int GetServerPort() const override
    {
        return m_boundPort.load();
    }

private:
    void ConfigureRoutes(httplib::Server& svr)
    {
        svr.set_default_file_mimetype("application/octet-stream");
        svr.set_file_extension_and_mimetype_mapping("usda", "text/plain; charset=utf-8");
        svr.set_file_extension_and_mimetype_mapping("usdz", "model/vnd.usdz+zip");
        svr.set_file_extension_and_mimetype_mapping("json", "application/json; charset=utf-8");
        svr.set_file_extension_and_mimetype_mapping("gltf", "model/gltf+json");
        svr.set_file_extension_and_mimetype_mapping("glb", "model/gltf-binary");
        svr.set_file_extension_and_mimetype_mapping("obj", "text/plain; charset=utf-8");
        svr.set_file_extension_and_mimetype_mapping("mtl", "text/plain; charset=utf-8");

        svr.Get("/dirs", [this](const httplib::Request&, httplib::Response& res) {
            nlohmann::json body;
            body["directories"] = GetDirectories();
            res.status = 200;
            res.set_content(body.dump(), "application/json; charset=utf-8");
        });

        svr.Get("/list", [this](const httplib::Request& req, httplib::Response& res) {
            const std::string rawDir = req.has_param("dir") ? req.get_param_value("dir") : std::string();
            const std::optional<std::string> normalized = NormalizeRelativeDir(rawDir);
            if (!normalized.has_value()) {
                nlohmann::json errorBody = {
                    { "error", "Invalid directory path" },
                };
                res.status = 400;
                res.set_content(errorBody.dump(), "application/json; charset=utf-8");
                return;
            }

            const std::optional<DirectoryContents> contents = ListDirectory(*normalized);
            if (!contents.has_value()) {
                nlohmann::json errorBody = {
                    { "error", "Directory not found" },
                    { "dir", *normalized },
                };
                res.status = 404;
                res.set_content(errorBody.dump(), "application/json; charset=utf-8");
                return;
            }

            const std::filesystem::path rootDir = GetRootDirectory();
            if (!rootDir.empty() && !contents->files.empty()) {
                const std::filesystem::path dirDiskPath =
                    normalized->empty() ? rootDir : (rootDir / PathFromUtf8(*normalized));
                m_screenshotPool.EnqueueDirectoryFiles(dirDiskPath, contents->files);
            }

            nlohmann::json body = {
                { "dir", *normalized },
                { "directories", contents->directories },
                { "files", contents->files },
            };
            res.status = 200;
            res.set_content(body.dump(), "application/json; charset=utf-8");
        });

        svr.Get(R"(/.*)", [this](const httplib::Request& req, httplib::Response& res) {
            const std::filesystem::path rootDir = GetRootDirectory();
            if (rootDir.empty()) {
                res.status = 404;
                return;
            }

            if (req.path.find('\0') != std::string::npos || req.path.find(':') != std::string::npos) {
                res.status = 400;
                return;
            }

            const std::optional<std::string> normalized = NormalizeRelativeDir(req.path);
            if (!normalized.has_value()) {
                res.status = 400;
                return;
            }

            std::filesystem::path targetPath =
                normalized->empty() ? rootDir : (rootDir / PathFromUtf8(*normalized));

            std::error_code ec;
            if (std::filesystem::is_directory(targetPath, ec) && !ec) {
                if (!req.path.empty() && req.path.back() != '/') {
                    res.set_redirect(req.path + "/", httplib::StatusCode::MovedPermanently_301);
                    return;
                }
                targetPath /= "index.html";
            }

            if (ToLowerAscii(PathToUtf8(targetPath.extension())) == ".jpg")
                m_screenshotPool.WaitForPendingScreenshot(targetPath);

            ec.clear();
            if (!std::filesystem::is_regular_file(targetPath, ec) || ec) {
                res.status = 404;
                return;
            }

            const std::string targetUtf8 = PathToUtf8(targetPath);
            httplib::detail::FileStat stat(targetUtf8);
            if (!stat.is_file()) {
                res.status = 404;
                return;
            }

            const std::string etag = httplib::detail::compute_etag(stat);
            if (!etag.empty())
                res.set_header("ETag", etag);

            const std::string lastModified = httplib::detail::file_mtime_to_http_date(stat.mtime());
            if (!lastModified.empty())
                res.set_header("Last-Modified", lastModified);

            if (req.has_header("If-None-Match") && !etag.empty() &&
                req.get_header_value("If-None-Match") == etag) {
                res.status = 304;
                return;
            }

            res.set_file_content(targetUtf8);
        });
    }

    bool BindServerLocked(const std::string& host, int port, std::unique_ptr<httplib::Server>& outServer)
    {
        if (m_running.load())
            return false;

        if (m_serverThread.joinable())
            m_serverThread.join();

        auto svr = std::make_unique<httplib::Server>();
        ConfigureRoutes(*svr);

        int resolvedPort = 0;
        if (port == 0) {
            resolvedPort = svr->bind_to_any_port(host);
            if (resolvedPort <= 0)
                return false;
        }
        else if (port > 0 && port <= 65535) {
            if (!svr->bind_to_port(host, port))
                return false;
            resolvedPort = port;
        }
        else {
            return false;
        }

        m_boundPort.store(resolvedPort);
        outServer = std::move(svr);
        return true;
    }

    void ScanDirectoryTask(
        const std::filesystem::path& currentDir,
        const std::string& relativeDirUtf8,
        const std::unordered_set<std::string>& supportedExtensions,
        tbb::concurrent_vector<std::pair<std::string, std::vector<std::string>>>& outMatchedDirFiles)
    {
        std::error_code ec;
        std::filesystem::directory_iterator iter(
            currentDir, std::filesystem::directory_options::skip_permission_denied, ec);
        if (ec)
            return;

        std::vector<std::string> matchedFiles;

        for (const std::filesystem::directory_entry& entry : iter) {
            std::error_code entryEc;
            if (entry.is_symlink(entryEc)) {
                if (entry.is_directory(entryEc))
                    continue;
            }

            const std::filesystem::path entryPath = entry.path();
            const std::string entryNameUtf8 = PathToUtf8(entryPath.filename());
            if (entryNameUtf8.empty())
                continue;

            if (entry.is_directory(entryEc) && !entryEc) {
                std::string childRelative =
                    relativeDirUtf8.empty() ? entryNameUtf8 : (relativeDirUtf8 + "/" + entryNameUtf8);
                m_scanTaskGroup.run([this,
                                     entryPath,
                                     childRel = std::move(childRelative),
                                     &supportedExtensions,
                                     &outMatchedDirFiles]() {
                    ScanDirectoryTask(entryPath, childRel, supportedExtensions, outMatchedDirFiles);
                });
            }
            else if (entry.is_regular_file(entryEc) && !entryEc) {
                if (IsSupportedAssetFile(entryPath, entryNameUtf8, supportedExtensions))
                    matchedFiles.push_back(entryNameUtf8);
            }
        }

        if (!matchedFiles.empty()) {
            std::sort(matchedFiles.begin(), matchedFiles.end());
            outMatchedDirFiles.emplace_back(relativeDirUtf8, std::move(matchedFiles));
        }
    }

    std::mutex m_scanMutex;
    tbb::task_group m_scanTaskGroup;

    mutable std::shared_mutex m_dataMutex;
    std::filesystem::path m_rootDirectory;
    std::map<std::string, DirectoryContents> m_directories;

    mutable std::mutex m_serverMutex;
    std::unique_ptr<httplib::Server> m_server;
    std::thread m_serverThread;
    std::atomic<int> m_boundPort{ 0 };
    std::atomic<bool> m_running{ false };
    ScreenshotWorkerPool m_screenshotPool;
};

} // namespace

IAssetDatabasePtr CreateAssetDatabase()
{
    auto* instance = new AssetDatabase();
    return IAssetDatabasePtr(instance, [](IAssetDatabase* p) {
        if (p)
            p->Release();
    });
}

IAssetDatabasePtr CreateAssetDatabase(const std::filesystem::path& rootDirectory)
{
    IAssetDatabasePtr db = CreateAssetDatabase();
    if (!db->Scan(rootDirectory))
        return nullptr;
    return db;
}
