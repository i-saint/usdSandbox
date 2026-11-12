#include "assetDatabase.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

static HANDLE g_shutdownEvent = nullptr;

static BOOL WINAPI ConsoleCtrlHandler(DWORD ctrlType)
{
    switch (ctrlType) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
    case CTRL_CLOSE_EVENT:
    case CTRL_SHUTDOWN_EVENT:
        if (g_shutdownEvent)
            ::SetEvent(g_shutdownEvent);
        return TRUE;
    default:
        return FALSE;
    }
}

static std::string WideToUtf8(std::wstring_view wide)
{
    if (wide.empty())
        return {};
    const int len = ::WideCharToMultiByte(
        CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    if (len <= 0)
        return {};
    std::string utf8(static_cast<size_t>(len), '\0');
    ::WideCharToMultiByte(
        CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), utf8.data(), len, nullptr, nullptr);
    return utf8;
}

static std::filesystem::path PathFromUtf8(std::string_view utf8)
{
    if (utf8.empty())
        return {};
    const int wideLen = ::MultiByteToWideChar(
        CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (wideLen <= 0)
        return {};
    std::wstring wide(static_cast<size_t>(wideLen), L'\0');
    ::MultiByteToWideChar(
        CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), wide.data(), wideLen);
    return std::filesystem::path(wide);
}

static std::string PathToUtf8(const std::filesystem::path& path)
{
    return WideToUtf8(path.native());
}

struct CliOptions
{
    std::filesystem::path assetsDir;
    bool assetsDirExplicit = false;

    std::filesystem::path htmlDir;
    bool htmlDirExplicit = false;

    size_t workerCount = 4;
    IAssetDatabase::LogLevel logLevel = IAssetDatabase::LogLevel::Info;
    std::string host = "127.0.0.1";
    int port = 8080;
    bool showHelp = false;
};

static std::string ToLowerAscii(std::string_view input)
{
    std::string lower(input.size(), '\0');
    std::transform(input.begin(), input.end(), lower.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return lower;
}

static const char* LogLevelToString(IAssetDatabase::LogLevel level)
{
    switch (level) {
    case IAssetDatabase::LogLevel::Silent: return "silent";
    case IAssetDatabase::LogLevel::Error:  return "error";
    case IAssetDatabase::LogLevel::Info:   return "info";
    case IAssetDatabase::LogLevel::Debug:  return "debug";
    default:                               return "info";
    }
}

static bool ParseLogLevel(std::string_view value, IAssetDatabase::LogLevel* outLevel)
{
    const std::string lower = ToLowerAscii(value);
    if (lower == "silent" || lower == "none" || lower == "off" || lower == "0") {
        *outLevel = IAssetDatabase::LogLevel::Silent;
        return true;
    }
    if (lower == "error" || lower == "1") {
        *outLevel = IAssetDatabase::LogLevel::Error;
        return true;
    }
    if (lower == "info" || lower == "default" || lower == "2") {
        *outLevel = IAssetDatabase::LogLevel::Info;
        return true;
    }
    if (lower == "debug" || lower == "verbose" || lower == "3") {
        *outLevel = IAssetDatabase::LogLevel::Debug;
        return true;
    }
    return false;
}

static void PrintUsage()
{
    std::cout <<
        "Usage:\n"
        "  usdAssetDatabase [options] [assetsDir] [htmlDir]\n"
        "\n"
        "Options:\n"
        "  -a, --assets <dir>       Asset root directory (default: ./assets)\n"
        "  -w, --html <dir>         HTTP static file root directory (default: ./html)\n"
        "  -j, --jobs <N>           Number of usdScreenshot worker processes (default: 4)\n"
        "  -l, --logLevel <level>   Log level: silent, error, info, debug (default: info)\n"
        "  -q, --quiet              Alias for --logLevel silent\n"
        "  -v, --verbose            Alias for --logLevel debug\n"
        "      --host <addr>        HTTP bind address (default: 127.0.0.1)\n"
        "  -p, --port <port>        HTTP listen port (default: 8080, 0 = auto)\n"
        "  -h, --help               Show this help message\n";
}

static bool ParseCommandLine(
    const std::vector<std::string>& args,
    const std::filesystem::path& currentDir,
    CliOptions* outOptions,
    std::string* outError)
{
    outOptions->assetsDir = currentDir / L"assets";
    outOptions->htmlDir = currentDir / L"html";

    std::vector<std::string> positional;

    for (size_t i = 1; i < args.size(); ++i) {
        const std::string& arg = args[i];

        auto requireValue = [&](const char* flagName, std::string* outVal) -> bool {
            if (i + 1 >= args.size()) {
                *outError = std::string("Missing value after ") + flagName;
                return false;
            }
            *outVal = args[++i];
            return true;
        };

        if (arg == "-h" || arg == "--help" || arg == "/?") {
            outOptions->showHelp = true;
            return true;
        }
        if (arg == "-a" || arg == "--assets" || arg == "--assetDir") {
            std::string val;
            if (!requireValue(arg.c_str(), &val))
                return false;
            outOptions->assetsDir = PathFromUtf8(val);
            outOptions->assetsDirExplicit = true;
        }
        else if (arg == "-w" || arg == "--html" || arg == "--htmlDir" || arg == "--serverRoot") {
            std::string val;
            if (!requireValue(arg.c_str(), &val))
                return false;
            outOptions->htmlDir = PathFromUtf8(val);
            outOptions->htmlDirExplicit = true;
        }
        else if (arg == "-j" || arg == "--jobs" || arg == "--workers") {
            std::string val;
            if (!requireValue(arg.c_str(), &val))
                return false;
            size_t jobs = 0;
            const auto res = std::from_chars(val.data(), val.data() + val.size(), jobs);
            if (res.ec != std::errc{} || res.ptr != val.data() + val.size()) {
                *outError = "Invalid --jobs value: " + val;
                return false;
            }
            outOptions->workerCount = jobs;
        }
        else if (arg == "-l" || arg == "--logLevel" || arg == "--log-level") {
            std::string val;
            if (!requireValue(arg.c_str(), &val))
                return false;
            if (!ParseLogLevel(val, &outOptions->logLevel)) {
                *outError = "Invalid --logLevel value (expected silent, error, info, or debug): " + val;
                return false;
            }
        }
        else if (arg == "-q" || arg == "--quiet") {
            outOptions->logLevel = IAssetDatabase::LogLevel::Silent;
        }
        else if (arg == "-v" || arg == "--verbose") {
            outOptions->logLevel = IAssetDatabase::LogLevel::Debug;
        }
        else if (arg == "--host") {
            if (!requireValue(arg.c_str(), &outOptions->host))
                return false;
        }
        else if (arg == "-p" || arg == "--port") {
            std::string val;
            if (!requireValue(arg.c_str(), &val))
                return false;
            int port = -1;
            const auto res = std::from_chars(val.data(), val.data() + val.size(), port);
            if (res.ec != std::errc{} || res.ptr != val.data() + val.size() || port < 0 || port > 65535) {
                *outError = "Invalid --port value: " + val;
                return false;
            }
            outOptions->port = port;
        }
        else if (!arg.empty() && arg.front() == '-') {
            *outError = "Unknown option: " + arg;
            return false;
        }
        else {
            positional.push_back(arg);
        }
    }

    if (positional.size() >= 1 && !outOptions->assetsDirExplicit) {
        outOptions->assetsDir = PathFromUtf8(positional[0]);
        outOptions->assetsDirExplicit = true;
    }
    else if (positional.size() >= 1 && outOptions->assetsDirExplicit && !outOptions->htmlDirExplicit) {
        outOptions->htmlDir = PathFromUtf8(positional[0]);
        outOptions->htmlDirExplicit = true;
    }

    if (positional.size() >= 2 && !outOptions->htmlDirExplicit) {
        outOptions->htmlDir = PathFromUtf8(positional[1]);
        outOptions->htmlDirExplicit = true;
    }

    if (positional.size() > 2) {
        *outError = "Too many positional arguments.";
        return false;
    }

    return true;
}

int wmain(int argc, wchar_t* argv[])
{
    ::SetConsoleOutputCP(CP_UTF8);

    std::vector<std::string> utf8Args;
    utf8Args.reserve(static_cast<size_t>(argc));
    for (int i = 0; i < argc; ++i)
        utf8Args.push_back(WideToUtf8(argv[i]));

    std::error_code cwdEc;
    const std::filesystem::path currentDir = std::filesystem::current_path(cwdEc);
    CliOptions options;
    std::string parseError;
    if (!ParseCommandLine(utf8Args, currentDir, &options, &parseError)) {
        std::cerr << "Error: " << parseError << "\n\n";
        PrintUsage();
        return 2;
    }

    if (options.showHelp) {
        PrintUsage();
        return 0;
    }

    IAssetDatabasePtr db = CreateAssetDatabase();
    db->SetLogLevel(options.logLevel);
    db->SetScreenshotWorkerCount(options.workerCount);

    if (!db->Scan(options.assetsDir)) {
        if (options.logLevel >= IAssetDatabase::LogLevel::Error) {
            std::cerr << "Error: Assets directory does not exist or is not a directory: "
                      << PathToUtf8(options.assetsDir) << "\n";
        }
        return 1;
    }

    std::error_code ec;
    if (std::filesystem::exists(options.htmlDir, ec) && std::filesystem::is_directory(options.htmlDir, ec)) {
        if (!db->SetServerRootDirectory(options.htmlDir)) {
            if (options.logLevel >= IAssetDatabase::LogLevel::Error) {
                std::cerr << "Error: Failed to set HTML root directory: "
                          << PathToUtf8(options.htmlDir) << "\n";
            }
            return 1;
        }
    }
    else if (options.htmlDirExplicit) {
        if (options.logLevel >= IAssetDatabase::LogLevel::Error) {
            std::cerr << "Error: HTML directory does not exist or is not a directory: "
                      << PathToUtf8(options.htmlDir) << "\n";
        }
        return 1;
    }
    else if (options.logLevel >= IAssetDatabase::LogLevel::Info) {
        std::cout << "Note: Default HTML directory not found ("
                  << PathToUtf8(options.htmlDir) << "); static web root disabled.\n";
    }

    g_shutdownEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    ::SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);

    if (!db->StartServer(options.host, options.port)) {
        if (options.logLevel >= IAssetDatabase::LogLevel::Error) {
            std::cerr << "Error: Failed to start HTTP server on "
                      << options.host << ":" << options.port << "\n";
        }
        ::SetConsoleCtrlHandler(ConsoleCtrlHandler, FALSE);
        if (g_shutdownEvent) {
            ::CloseHandle(g_shutdownEvent);
            g_shutdownEvent = nullptr;
        }
        return 1;
    }

    if (options.logLevel >= IAssetDatabase::LogLevel::Info) {
        const int boundPort = db->GetServerPort();
        std::cout << "USD Asset Database Server listening on http://"
                  << options.host << ":" << boundPort << "\n"
                  << "  Assets root : " << PathToUtf8(db->GetRootDirectory())
                  << " (" << db->GetDirectories().size() << " directories)\n"
                  << "  HTML root   : "
                  << (db->GetServerRootDirectory().empty()
                          ? std::string("(disabled)")
                          : PathToUtf8(db->GetServerRootDirectory()))
                  << "\n"
                  << "  Workers     : " << db->GetScreenshotWorkerCount() << "\n"
                  << "  Log level   : " << LogLevelToString(db->GetLogLevel()) << "\n"
                  << "Press Ctrl+C to stop.\n" << std::flush;
    }

    if (g_shutdownEvent)
        ::WaitForSingleObject(g_shutdownEvent, INFINITE);

    if (options.logLevel >= IAssetDatabase::LogLevel::Info)
        std::cout << "Stopping server...\n" << std::flush;
    db->StopServer();

    ::SetConsoleCtrlHandler(ConsoleCtrlHandler, FALSE);
    if (g_shutdownEvent) {
        ::CloseHandle(g_shutdownEvent);
        g_shutdownEvent = nullptr;
    }

    return 0;
}
