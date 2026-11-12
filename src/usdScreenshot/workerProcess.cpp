#include "workerProcess.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <fcntl.h>
#include <io.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string_view>
#include <thread>
#include <vector>

namespace {

constexpr std::string_view kPrefixReady = "@@READY";
constexpr std::string_view kPrefixOk = "@@OK";
constexpr std::string_view kPrefixErr = "@@ERR";
constexpr std::string_view kCommandQuit = "@@QUIT";

std::string SanitizeSingleLine(std::string text)
{
    for (char& ch : text) {
        if (ch == '\r' || ch == '\n' || ch == '\t')
            ch = ' ';
    }
    return text;
}

std::wstring Utf8ToWide(const std::string& utf8)
{
    if (utf8.empty())
        return {};
    const int len = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (len <= 0)
        return {};
    std::wstring wide(static_cast<size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), wide.data(), len);
    return wide;
}

std::wstring GetSelfExecutablePath()
{
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
        const DWORD len = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (len == 0)
            return {};
        if (len < buffer.size() - 1) {
            buffer.resize(len);
            return buffer;
        }
        buffer.resize(buffer.size() * 2);
    }
}

std::wstring QuoteWin32Arg(std::wstring_view arg)
{
    if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring_view::npos)
        return std::wstring(arg);

    std::wstring quoted = L"\"";
    for (auto it = arg.begin(); ; ++it) {
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

std::wstring BuildWorkerCommandLine(const std::wstring& exePath, const RenderOptions& opts)
{
    std::vector<std::wstring> args;
    args.push_back(exePath);
    args.push_back(L"--worker");
    args.push_back(L"--imageWidth");
    args.push_back(std::to_wstring(opts.imageWidth));

    {
        std::wostringstream oss;
        oss << std::setprecision(6) << opts.complexity;
        args.push_back(L"--complexity");
        args.push_back(oss.str());
    }

    if (!opts.colorCorrectionMode.empty()) {
        args.push_back(L"--colorCorrectionMode");
        args.push_back(Utf8ToWide(opts.colorCorrectionMode));
    }
    if (!opts.rendererPlugin.empty()) {
        args.push_back(L"--renderer");
        args.push_back(Utf8ToWide(opts.rendererPlugin));
    }
    if (!opts.camera.empty()) {
        args.push_back(L"--camera");
        args.push_back(Utf8ToWide(opts.camera));
    }
    if (opts.timeCode.has_value()) {
        std::wostringstream oss;
        oss << std::setprecision(17) << *opts.timeCode;
        args.push_back(L"--time");
        args.push_back(oss.str());
    }
    else if (opts.earliestTime) {
        args.push_back(L"--time");
        args.push_back(L"earliest");
    }

    if (!opts.gpuEnabled)
        args.push_back(L"--disableGpu");
    if (!opts.enableUsdDrawModes)
        args.push_back(L"--disableDrawMode");
    if (!opts.cameraLightEnabled)
        args.push_back(L"--disableCameraLight");
    if (opts.domeLightVisibility)
        args.push_back(L"--enableDomeLightVisibility");

    {
        std::string joinedPurposes;
        for (size_t i = 0; i < opts.purposes.size(); ++i) {
            if (i > 0)
                joinedPurposes.push_back(',');
            joinedPurposes += opts.purposes[i];
        }
        args.push_back(L"--purposes");
        args.push_back(Utf8ToWide(joinedPurposes));
    }

    if (opts.threadsPerWorker > 0) {
        args.push_back(L"--threadsPerWorker");
        args.push_back(std::to_wstring(opts.threadsPerWorker));
    }

    std::wstring cmdLine;
    for (size_t i = 0; i < args.size(); ++i) {
        if (i > 0)
            cmdLine.push_back(L' ');
        cmdLine += QuoteWin32Arg(args[i]);
    }
    return cmdLine;
}

class ScopedJobObject
{
public:
    ScopedJobObject()
    {
        m_job = CreateJobObjectW(nullptr, nullptr);
        if (m_job) {
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
            info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            SetInformationJobObject(m_job, JobObjectExtendedLimitInformation, &info, sizeof(info));
        }
    }

    ~ScopedJobObject()
    {
        if (m_job)
            CloseHandle(m_job);
    }

    ScopedJobObject(const ScopedJobObject&) = delete;
    ScopedJobObject& operator=(const ScopedJobObject&) = delete;

    void Assign(HANDLE process) const
    {
        if (m_job && process)
            AssignProcessToJobObject(m_job, process);
    }

private:
    HANDLE m_job = nullptr;
};

class WorkerClient
{
public:
    WorkerClient() = default;
    ~WorkerClient()
    {
        Shutdown(true);
    }

    WorkerClient(const WorkerClient&) = delete;
    WorkerClient& operator=(const WorkerClient&) = delete;

    bool Start(
        const std::wstring& commandLine,
        const ScopedJobObject& jobObject,
        bool verbose,
        std::mutex& spawnMutex,
        std::string* errorMessage)
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

            if (!CreatePipe(&stdinRead, &stdinWrite, &saInherit, 0)) {
                if (errorMessage)
                    *errorMessage = "CreatePipe(stdin) failed.";
                return false;
            }
            SetHandleInformation(stdinWrite, HANDLE_FLAG_INHERIT, 0);

            if (!CreatePipe(&stdoutRead, &stdoutWrite, &saInherit, 0)) {
                CloseHandle(stdinRead);
                CloseHandle(stdinWrite);
                if (errorMessage)
                    *errorMessage = "CreatePipe(stdout) failed.";
                return false;
            }
            SetHandleInformation(stdoutRead, HANDLE_FLAG_INHERIT, 0);

            if (verbose) {
                HANDLE parentStderr = GetStdHandle(STD_ERROR_HANDLE);
                if (parentStderr && parentStderr != INVALID_HANDLE_VALUE) {
                    if (DuplicateHandle(
                            GetCurrentProcess(),
                            parentStderr,
                            GetCurrentProcess(),
                            &stderrHandle,
                            0,
                            TRUE,
                            DUPLICATE_SAME_ACCESS)) {
                        closeStderrHandle = true;
                    }
                }
            }
            if (!stderrHandle) {
                stderrHandle = CreateFileW(
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
            }

            HANDLE inheritedHandles[3] = { stdinRead, stdoutWrite, stderrHandle };
            DWORD inheritedCount = stderrHandle ? 3 : 2;
            // Deduplicate handles if any match
            if (inheritedCount == 3 && (inheritedHandles[2] == inheritedHandles[0] || inheritedHandles[2] == inheritedHandles[1]))
                inheritedCount = 2;

            SIZE_T attrListSize = 0;
            InitializeProcThreadAttributeList(nullptr, 1, 0, &attrListSize);
            std::vector<unsigned char> attrListBuffer(attrListSize);
            auto* attrList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attrListBuffer.data());

            if (!InitializeProcThreadAttributeList(attrList, 1, 0, &attrListSize) ||
                !UpdateProcThreadAttribute(
                    attrList,
                    0,
                    PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                    inheritedHandles,
                    inheritedCount * sizeof(HANDLE),
                    nullptr,
                    nullptr)) {
                if (attrList)
                    DeleteProcThreadAttributeList(attrList);
                CloseHandle(stdinRead);
                CloseHandle(stdinWrite);
                CloseHandle(stdoutRead);
                CloseHandle(stdoutWrite);
                if (closeStderrHandle && stderrHandle)
                    CloseHandle(stderrHandle);
                if (errorMessage)
                    *errorMessage = "Failed to initialize process handle attribute list.";
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
            PROCESS_INFORMATION pi{};
            const BOOL created = CreateProcessW(
                nullptr,
                mutableCmd.data(),
                nullptr,
                nullptr,
                TRUE,
                EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW,
                nullptr,
                nullptr,
                &si.StartupInfo,
                &pi);

            DeleteProcThreadAttributeList(attrList);
            CloseHandle(stdinRead);
            CloseHandle(stdoutWrite);
            if (closeStderrHandle && stderrHandle)
                CloseHandle(stderrHandle);

            if (!created) {
                CloseHandle(stdinWrite);
                CloseHandle(stdoutRead);
                if (errorMessage)
                    *errorMessage = "CreateProcessW failed for worker (error " + std::to_string(GetLastError()) + ").";
                return false;
            }

            jobObject.Assign(pi.hProcess);
            CloseHandle(pi.hThread);

            m_process = pi.hProcess;
            m_stdinWrite = stdinWrite;
            m_stdoutRead = stdoutRead;
            m_readBuffer.clear();
        }

        // Wait for @@READY handshake from worker
        std::string line;
        while (ReadLine(&line)) {
            if (line == kPrefixReady)
                return true;
            if (line.starts_with(kPrefixErr)) {
                const size_t tabPos = line.find('\t');
                if (errorMessage) {
                    *errorMessage = (tabPos != std::string::npos)
                        ? line.substr(tabPos + 1)
                        : "Worker initialization failed.";
                }
                Shutdown(true);
                return false;
            }
        }

        if (errorMessage)
            *errorMessage = "Worker exited before sending ready signal (exit code " + std::to_string(GetExitCode()) + ").";
        Shutdown(true);
        return false;
    }

    bool ExecuteTask(
        const ScreenshotTask& task,
        bool* outWorkerCrashed,
        std::string* outErrorMessage)
    {
        if (outWorkerCrashed)
            *outWorkerCrashed = false;

        std::string payload = SanitizeSingleLine(task.usdFilePath);
        payload.push_back('\t');
        payload += SanitizeSingleLine(task.outputImagePath);
        payload.push_back('\n');

        if (!WriteAll(payload)) {
            if (outWorkerCrashed)
                *outWorkerCrashed = true;
            if (outErrorMessage)
                *outErrorMessage = "Failed to write task to worker pipe.";
            return false;
        }

        std::string line;
        while (ReadLine(&line)) {
            if (line == kPrefixOk)
                return true;
            if (line.starts_with(kPrefixErr)) {
                const size_t tabPos = line.find('\t');
                if (outErrorMessage) {
                    *outErrorMessage = (tabPos != std::string::npos)
                        ? line.substr(tabPos + 1)
                        : "Worker reported error.";
                }
                return false;
            }
        }

        if (outWorkerCrashed)
            *outWorkerCrashed = true;
        if (outErrorMessage) {
            std::ostringstream oss;
            oss << "Worker process terminated unexpectedly (exit code 0x"
                << std::hex << std::uppercase << GetExitCode() << ").";
            *outErrorMessage = oss.str();
        }
        return false;
    }

    bool IsRunning() const
    {
        return m_process != nullptr && m_stdinWrite != nullptr && m_stdoutRead != nullptr;
    }

    void Shutdown(bool forceTerminateIfAlive)
    {
        if (m_stdinWrite) {
            if (!forceTerminateIfAlive) {
                const std::string quitCmd = std::string(kCommandQuit) + "\n";
                WriteAll(quitCmd);
            }
            CloseHandle(m_stdinWrite);
            m_stdinWrite = nullptr;
        }
        if (m_stdoutRead) {
            CloseHandle(m_stdoutRead);
            m_stdoutRead = nullptr;
        }
        if (m_process) {
            const DWORD waitMs = forceTerminateIfAlive ? 100 : 5000;
            if (WaitForSingleObject(m_process, waitMs) != WAIT_OBJECT_0)
                TerminateProcess(m_process, 1);
            CloseHandle(m_process);
            m_process = nullptr;
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
            if (!WriteFile(m_stdinWrite, data.data() + offset, chunk, &written, nullptr) || written == 0)
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
            if (!ReadFile(m_stdoutRead, chunk, sizeof(chunk), &bytesRead, nullptr) || bytesRead == 0) {
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

    DWORD GetExitCode() const
    {
        if (!m_process)
            return 0;
        DWORD code = 0;
        if (GetExitCodeProcess(m_process, &code))
            return code;
        return 0;
    }

    HANDLE m_process = nullptr;
    HANDLE m_stdinWrite = nullptr;
    HANDLE m_stdoutRead = nullptr;
    std::string m_readBuffer;
};

} // namespace

int RunWorkerMode(const RenderOptions& renderOptions)
{
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);

    ScreenshotRenderer renderer;
    std::string initError;
    if (!renderer.Initialize(renderOptions, &initError)) {
        std::cout << kPrefixErr << '\t' << SanitizeSingleLine(initError) << '\n' << std::flush;
        return 1;
    }

    std::cout << kPrefixReady << '\n' << std::flush;

    std::string line;
    while (std::getline(std::cin, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.empty())
            continue;
        if (line == kCommandQuit)
            break;

        const size_t tabPos = line.find('\t');
        if (tabPos == std::string::npos) {
            std::cout << kPrefixErr << "\tInvalid request format (expected <usdPath>\\t<outputPath>)\n" << std::flush;
            continue;
        }

        const std::string usdFilePath = line.substr(0, tabPos);
        const std::string outputImagePath = line.substr(tabPos + 1);
        if (usdFilePath.empty() || outputImagePath.empty()) {
            std::cout << kPrefixErr << "\tEmpty input or output path\n" << std::flush;
            continue;
        }

        std::string renderError;
        if (renderer.Render(usdFilePath, outputImagePath, &renderError)) {
            std::cout << kPrefixOk << '\n' << std::flush;
        }
        else {
            std::cout << kPrefixErr << '\t' << SanitizeSingleLine(renderError) << '\n' << std::flush;
        }
    }

    return 0;
}

bool RunCoordinatorMode(
    const std::vector<ScreenshotTask>& tasks,
    const RenderOptions& renderOptions,
    const CoordinatorOptions& coordinatorOptions,
    BatchSummary* outSummary)
{
    BatchSummary summary{};
    summary.totalCount = tasks.size();

    if (tasks.empty()) {
        if (outSummary)
            *outSummary = summary;
        return true;
    }

    const std::wstring selfExe = GetSelfExecutablePath();
    if (selfExe.empty()) {
        std::cerr << "Error: Failed to resolve executable path.\n";
        if (outSummary)
            *outSummary = summary;
        return false;
    }

    const unsigned int hwThreads = std::max(1u, std::thread::hardware_concurrency());
    unsigned int workerCount = coordinatorOptions.workerCount;
    if (workerCount == 0)
        workerCount = std::clamp(hwThreads / 2, 1u, 8u);
    workerCount = static_cast<unsigned int>(std::min<size_t>(workerCount, tasks.size()));

    RenderOptions effectiveRenderOptions = renderOptions;
    if (effectiveRenderOptions.threadsPerWorker == 0)
        effectiveRenderOptions.threadsPerWorker = std::max(1u, hwThreads / workerCount);

    const std::wstring workerCmdLine = BuildWorkerCommandLine(selfExe, effectiveRenderOptions);

    ScopedJobObject jobObject;
    std::mutex spawnMutex;
    std::mutex logMutex;

    std::atomic<size_t> nextTaskIndex{ 0 };
    std::atomic<size_t> completedCount{ 0 };
    std::atomic<size_t> succeededCount{ 0 };
    std::atomic<size_t> failedCount{ 0 };
    std::atomic<bool> fatalInitFailure{ false };

    auto workerThreadProc = [&]() {
        WorkerClient client;

        for (;;) {
            if (fatalInitFailure.load(std::memory_order_relaxed))
                break;

            const size_t index = nextTaskIndex.fetch_add(1, std::memory_order_relaxed);
            if (index >= tasks.size())
                break;

            const ScreenshotTask& task = tasks[index];

            if (!client.IsRunning()) {
                std::string startError;
                if (!client.Start(workerCmdLine, jobObject, coordinatorOptions.verbose, spawnMutex, &startError)) {
                    failedCount.fetch_add(1, std::memory_order_relaxed);
                    const size_t done = completedCount.fetch_add(1, std::memory_order_relaxed) + 1;
                    fatalInitFailure.store(true, std::memory_order_relaxed);

                    std::lock_guard<std::mutex> lock(logMutex);
                    std::cerr << "[" << done << "/" << tasks.size() << "] FAIL "
                              << task.usdFilePath << " -> " << startError << "\n";
                    break;
                }
            }

            bool workerCrashed = false;
            std::string taskError;
            const bool ok = client.ExecuteTask(task, &workerCrashed, &taskError);
            const size_t done = completedCount.fetch_add(1, std::memory_order_relaxed) + 1;

            if (ok) {
                succeededCount.fetch_add(1, std::memory_order_relaxed);
                if (!coordinatorOptions.quiet) {
                    std::lock_guard<std::mutex> lock(logMutex);
                    std::cout << "[" << done << "/" << tasks.size() << "] OK   "
                              << task.usdFilePath << " -> " << task.outputImagePath << "\n";
                }
            }
            else {
                failedCount.fetch_add(1, std::memory_order_relaxed);
                if (workerCrashed)
                    client.Shutdown(true);

                std::lock_guard<std::mutex> lock(logMutex);
                std::cerr << "[" << done << "/" << tasks.size() << "] FAIL "
                          << task.usdFilePath << " -> " << taskError << "\n";
            }
        }

        client.Shutdown(false);
    };

    std::vector<std::thread> threads;
    threads.reserve(workerCount);
    for (unsigned int i = 0; i < workerCount; ++i)
        threads.emplace_back(workerThreadProc);
    for (std::thread& t : threads)
        t.join();

    summary.succeededCount = succeededCount.load();
    summary.failedCount = failedCount.load();
    if (outSummary)
        *outSummary = summary;

    return summary.failedCount == 0 && !fatalInitFailure.load();
}
