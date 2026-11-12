#include "assetDatabase.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <httplib.h>
#include <nlohmann/json.hpp>

#pragma warning(push, 0)
#pragma warning(disable: 4244 4273 4305 4996)
#include <pxr/pxr.h>
#include <pxr/base/plug/registry.h>
#include <pxr/usd/sdf/fileFormat.h>
#pragma warning(pop)

#include <algorithm>
#include <atomic>
#include <cctype>
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

bool ScanDirectoryRecursive(
    const std::filesystem::path& currentDir,
    const std::string& relativeDirUtf8,
    const std::unordered_set<std::string>& supportedExtensions,
    std::map<std::string, IAssetDatabase::DirectoryContents>& outDirectories)
{
    std::error_code ec;
    std::filesystem::directory_iterator iter(
        currentDir, std::filesystem::directory_options::skip_permission_denied, ec);
    if (ec)
        return false;

    std::vector<std::pair<std::string, std::filesystem::path>> childDirs;
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
            childDirs.emplace_back(entryNameUtf8, entryPath);
        }
        else if (entry.is_regular_file(entryEc) && !entryEc) {
            if (IsSupportedAssetFile(entryPath, entryNameUtf8, supportedExtensions))
                matchedFiles.push_back(entryNameUtf8);
        }
    }

    std::sort(childDirs.begin(), childDirs.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.first < rhs.first;
    });
    std::sort(matchedFiles.begin(), matchedFiles.end());

    std::vector<std::string> keptChildDirs;
    keptChildDirs.reserve(childDirs.size());

    for (const auto& [childName, childPath] : childDirs) {
        const std::string childRelative =
            relativeDirUtf8.empty() ? childName : (relativeDirUtf8 + "/" + childName);
        if (ScanDirectoryRecursive(childPath, childRelative, supportedExtensions, outDirectories))
            keptChildDirs.push_back(childName);
    }

    if (matchedFiles.empty() && keptChildDirs.empty())
        return false;

    outDirectories.emplace(
        relativeDirUtf8,
        IAssetDatabase::DirectoryContents{ std::move(keptChildDirs), std::move(matchedFiles) });
    return true;
}

class AssetDatabase final : public IAssetDatabase
{
public:
    AssetDatabase() = default;

    ~AssetDatabase() override
    {
        StopServer();
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
        std::map<std::string, DirectoryContents> scannedDirectories;
        ScanDirectoryRecursive(effectiveRoot, "", supportedExtensions, scannedDirectories);

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

            m_server = std::move(svr);
            rawServer = m_server.get();
            m_running.store(true);
        }

        const bool ok = rawServer->listen_after_bind();

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

    mutable std::shared_mutex m_dataMutex;
    std::filesystem::path m_rootDirectory;
    std::map<std::string, DirectoryContents> m_directories;

    mutable std::mutex m_serverMutex;
    std::unique_ptr<httplib::Server> m_server;
    std::thread m_serverThread;
    std::atomic<int> m_boundPort{ 0 };
    std::atomic<bool> m_running{ false };
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
