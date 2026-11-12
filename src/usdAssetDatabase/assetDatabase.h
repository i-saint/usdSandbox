#pragma once

#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

class IAssetDatabase
{
public:
    struct DirectoryContents
    {
        std::vector<std::string> directories;
        std::vector<std::string> files;
    };

    virtual ~IAssetDatabase() = default;
    virtual void Release() = 0;

    virtual bool Scan(const std::filesystem::path& rootDirectory) = 0;
    virtual void Clear() = 0;

    virtual std::filesystem::path GetRootDirectory() const = 0;
    virtual std::vector<std::string> GetDirectories() const = 0;
    virtual std::optional<DirectoryContents> ListDirectory(std::string_view relativeDir = "") const = 0;

    virtual bool SetServerRootDirectory(const std::filesystem::path& serverRootDirectory) = 0;
    virtual std::filesystem::path GetServerRootDirectory() const = 0;

    virtual void SetScreenshotWorkerCount(size_t workerCount) = 0;
    virtual size_t GetScreenshotWorkerCount() const = 0;

    virtual bool StartServer(const std::string& host = "127.0.0.1", int port = 8080) = 0;
    virtual bool RunServer(const std::string& host = "127.0.0.1", int port = 8080) = 0;
    virtual void StopServer() = 0;
    virtual bool IsServerRunning() const = 0;
    virtual int GetServerPort() const = 0;
};
using IAssetDatabasePtr = std::shared_ptr<IAssetDatabase>;

IAssetDatabasePtr CreateAssetDatabase();
IAssetDatabasePtr CreateAssetDatabase(const std::filesystem::path& rootDirectory);
