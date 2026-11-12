#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#  ifdef USDASSETDATABASE_EXPORTS
#    define USDASSETDATABASE_API __declspec(dllexport)
#  else
#    define USDASSETDATABASE_API __declspec(dllimport)
#  endif
#else
#  define USDASSETDATABASE_API
#endif

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

    virtual bool StartServer(const std::string& host = "127.0.0.1", int port = 8080) = 0;
    virtual bool RunServer(const std::string& host = "127.0.0.1", int port = 8080) = 0;
    virtual void StopServer() = 0;
    virtual bool IsServerRunning() const = 0;
    virtual int GetServerPort() const = 0;
};
using IAssetDatabasePtr = std::shared_ptr<IAssetDatabase>;

USDASSETDATABASE_API IAssetDatabasePtr CreateAssetDatabase();
USDASSETDATABASE_API IAssetDatabasePtr CreateAssetDatabase(const std::filesystem::path& rootDirectory);
