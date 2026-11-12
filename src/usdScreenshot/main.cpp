#include "screenshotRenderer.h"
#include "workerProcess.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

static std::string WideToUtf8(std::wstring_view wide)
{
    if (wide.empty())
        return {};
    const int len = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    if (len <= 0)
        return {};
    std::string utf8(static_cast<size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), utf8.data(), len, nullptr, nullptr);
    return utf8;
}

static std::filesystem::path PathFromUtf8(const std::string& utf8)
{
    if (utf8.empty())
        return {};
    const int wideLen = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (wideLen <= 0)
        return std::filesystem::path(utf8);
    std::wstring wide(static_cast<size_t>(wideLen), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), wide.data(), wideLen);
    return std::filesystem::path(wide);
}

static std::string PathToUtf8(const std::filesystem::path& path)
{
    return WideToUtf8(path.native());
}

static std::string ToLowerAscii(std::string s)
{
    for (char& ch : s)
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return s;
}

static std::string Trim(std::string_view sv)
{
    while (!sv.empty() && (sv.front() == ' ' || sv.front() == '\t' || sv.front() == '\r' || sv.front() == '\n'))
        sv.remove_prefix(1);
    while (!sv.empty() && (sv.back() == ' ' || sv.back() == '\t' || sv.back() == '\r' || sv.back() == '\n'))
        sv.remove_suffix(1);
    return std::string(sv);
}

static std::vector<std::string> SplitCsvOrSpace(std::string_view text)
{
    std::vector<std::string> items;
    std::string current;
    for (char ch : text) {
        if (ch == ',' || ch == ' ' || ch == '\t') {
            if (!current.empty()) {
                items.push_back(std::move(current));
                current.clear();
            }
        }
        else {
            current.push_back(ch);
        }
    }
    if (!current.empty())
        items.push_back(std::move(current));
    return items;
}

static bool ParseComplexity(const std::string& value, float* outComplexity)
{
    const std::string lower = ToLowerAscii(value);
    if (lower == "low") {
        *outComplexity = 1.0f;
        return true;
    }
    if (lower == "medium") {
        *outComplexity = 1.1f;
        return true;
    }
    if (lower == "high") {
        *outComplexity = 1.2f;
        return true;
    }
    if (lower == "veryhigh") {
        *outComplexity = 1.3f;
        return true;
    }

    char* endPtr = nullptr;
    const float parsed = std::strtof(value.c_str(), &endPtr);
    if (endPtr != value.c_str() && *endPtr == '\0' && parsed >= 1.0f) {
        *outComplexity = parsed;
        return true;
    }
    return false;
}

static void PrintUsage()
{
    std::cout <<
        "Usage:\n"
        "  usdScreenshot <input.usd> <output.png> [options]\n"
        "  usdScreenshot <inputDir> <outputDir> [options]\n"
        "  usdScreenshot --list <tasks.txt> [-o <outputDir>] [options]\n"
        "  usdScreenshot <file1.usd> [file2.usd ...] -o <outputDir> [options]\n"
        "  usdScreenshot --worker [options]\n"
        "\n"
        "Batch & Process Options:\n"
        "  -j, --jobs <N>                  Number of worker processes (default: auto)\n"
        "  --threadsPerWorker <N>          TBB thread limit per worker (default: auto)\n"
        "  --list <file>                   UTF-8 task list (<usdPath>\\t<outPath> or <usdPath> per line)\n"
        "  -o, --outputDir <dir>           Output directory for --list or multiple input files\n"
        "  --format <ext>                  Output image extension for directory/list mode (default: png)\n"
        "  --ext <e1,e2,...>               Input extensions for directory scan (default: usd,usda,usdc,usdz)\n"
        "  --skipExisting                  Skip tasks whose output image already exists\n"
        "  --inProcess                     Render directly in the current process without spawning workers\n"
        "  --worker                        Run as stdin/stdout worker process\n"
        "  --verbose                       Forward worker stderr and extra diagnostics\n"
        "  -q, --quiet                     Suppress per-file success logs\n"
        "\n"
        "Render Options (usdrecord compatible):\n"
        "  -w, --imageWidth <px>           Output image width (default: 960)\n"
        "  --complexity <level|float>      low (1.0), medium (1.1), high (1.2), veryhigh (1.3)\n"
        "  --colorCorrectionMode <mode>    disabled, sRGB, openColorIO (default: sRGB)\n"
        "  -r, --renderer <name>           Hydra renderer plugin (default: Storm / GL)\n"
        "  -cam, --camera <pathOrName>     Camera prim path or prim name\n"
        "  --time <default|earliest|frame> TimeCode to render (default: default)\n"
        "  --purposes <p1,p2,...>          Additional UsdGeom purposes (default: proxy)\n"
        "  --disableGpu                    Disable GPU usage in Hydra\n"
        "  --disableDrawMode               Ignore USD draw modes\n"
        "  --disableCameraLight            Disable built-in camera headlight\n"
        "  --enableDomeLightVisibility     Draw dome light texture into background\n"
        "\n"
        "Worker Stdin/Stdout Protocol (--worker):\n"
        "  Startup output: @@READY\\n (or @@ERR\\t<reason>\\n)\n"
        "  Request line  : <usdFilePath>\\t<outputImagePath>\\n (or @@QUIT\\n)\n"
        "  Response line : @@OK\\n or @@ERR\\t<reason>\\n\n";
}

struct CliConfig
{
    RenderOptions render;
    CoordinatorOptions coordinator;
    bool workerMode = false;
    bool inProcessMode = false;
    bool skipExisting = false;
    bool showHelp = false;
    std::string listFilePath;
    std::string outputDir;
    std::string outputFormat = "png";
    std::vector<std::string> scanExtensions = { ".usd", ".usda", ".usdc", ".usdz" };
    std::vector<std::string> positionalArgs;
};

static bool ParseCommandLine(const std::vector<std::string>& args, CliConfig* cfg, std::string* errorMsg)
{
    for (size_t i = 1; i < args.size(); ++i) {
        const std::string& arg = args[i];

        auto requireValue = [&](const char* flagName, std::string* outVal) -> bool {
            if (i + 1 >= args.size()) {
                *errorMsg = std::string("Missing value after ") + flagName;
                return false;
            }
            *outVal = args[++i];
            return true;
        };

        if (arg == "-h" || arg == "--help" || arg == "/?") {
            cfg->showHelp = true;
            return true;
        }
        if (arg == "--worker") {
            cfg->workerMode = true;
        }
        else if (arg == "--inProcess") {
            cfg->inProcessMode = true;
        }
        else if (arg == "--skipExisting") {
            cfg->skipExisting = true;
        }
        else if (arg == "--verbose") {
            cfg->coordinator.verbose = true;
        }
        else if (arg == "-q" || arg == "--quiet") {
            cfg->coordinator.quiet = true;
        }
        else if (arg == "--disableGpu") {
            cfg->render.gpuEnabled = false;
        }
        else if (arg == "--disableDrawMode") {
            cfg->render.enableUsdDrawModes = false;
        }
        else if (arg == "--disableCameraLight") {
            cfg->render.cameraLightEnabled = false;
        }
        else if (arg == "--enableDomeLightVisibility") {
            cfg->render.domeLightVisibility = true;
        }
        else if (arg == "-j" || arg == "--jobs") {
            std::string val;
            if (!requireValue(arg.c_str(), &val))
                return false;
            unsigned int jobs = 0;
            const auto res = std::from_chars(val.data(), val.data() + val.size(), jobs);
            if (res.ec != std::errc{} || res.ptr != val.data() + val.size() || jobs == 0) {
                *errorMsg = "Invalid --jobs value: " + val;
                return false;
            }
            cfg->coordinator.workerCount = jobs;
        }
        else if (arg == "--threadsPerWorker") {
            std::string val;
            if (!requireValue(arg.c_str(), &val))
                return false;
            unsigned int threads = 0;
            const auto res = std::from_chars(val.data(), val.data() + val.size(), threads);
            if (res.ec != std::errc{} || res.ptr != val.data() + val.size()) {
                *errorMsg = "Invalid --threadsPerWorker value: " + val;
                return false;
            }
            cfg->render.threadsPerWorker = threads;
        }
        else if (arg == "-w" || arg == "--imageWidth" || arg == "--width") {
            std::string val;
            if (!requireValue(arg.c_str(), &val))
                return false;
            size_t width = 0;
            const auto res = std::from_chars(val.data(), val.data() + val.size(), width);
            if (res.ec != std::errc{} || res.ptr != val.data() + val.size() || width == 0) {
                *errorMsg = "Invalid --imageWidth value: " + val;
                return false;
            }
            cfg->render.imageWidth = width;
        }
        else if (arg == "--complexity") {
            std::string val;
            if (!requireValue(arg.c_str(), &val))
                return false;
            if (!ParseComplexity(val, &cfg->render.complexity)) {
                *errorMsg = "Invalid --complexity value: " + val;
                return false;
            }
        }
        else if (arg == "--colorCorrectionMode" || arg == "-color" || arg == "--color") {
            std::string val;
            if (!requireValue(arg.c_str(), &val))
                return false;
            if (val != "disabled" && val != "sRGB" && val != "openColorIO") {
                *errorMsg = "Invalid --colorCorrectionMode (expected disabled, sRGB, or openColorIO): " + val;
                return false;
            }
            cfg->render.colorCorrectionMode = val;
        }
        else if (arg == "-r" || arg == "--renderer") {
            if (!requireValue(arg.c_str(), &cfg->render.rendererPlugin))
                return false;
        }
        else if (arg == "-cam" || arg == "--camera") {
            if (!requireValue(arg.c_str(), &cfg->render.camera))
                return false;
        }
        else if (arg == "--time") {
            std::string val;
            if (!requireValue(arg.c_str(), &val))
                return false;
            const std::string lower = ToLowerAscii(val);
            if (lower == "default") {
                cfg->render.timeCode.reset();
                cfg->render.earliestTime = false;
            }
            else if (lower == "earliest" || lower == "start") {
                cfg->render.timeCode.reset();
                cfg->render.earliestTime = true;
            }
            else {
                char* endPtr = nullptr;
                const double frame = std::strtod(val.c_str(), &endPtr);
                if (endPtr == val.c_str() || *endPtr != '\0') {
                    *errorMsg = "Invalid --time value: " + val;
                    return false;
                }
                cfg->render.timeCode = frame;
                cfg->render.earliestTime = false;
            }
        }
        else if (arg == "--purposes") {
            std::string val;
            if (!requireValue(arg.c_str(), &val))
                return false;
            cfg->render.purposes = SplitCsvOrSpace(val);
        }
        else if (arg == "--list") {
            if (!requireValue(arg.c_str(), &cfg->listFilePath))
                return false;
        }
        else if (arg == "-o" || arg == "--outputDir") {
            if (!requireValue(arg.c_str(), &cfg->outputDir))
                return false;
        }
        else if (arg == "--format") {
            std::string val;
            if (!requireValue(arg.c_str(), &val))
                return false;
            if (!val.empty() && val.front() == '.')
                val.erase(val.begin());
            if (val.empty()) {
                *errorMsg = "Empty --format extension.";
                return false;
            }
            cfg->outputFormat = val;
        }
        else if (arg == "--ext") {
            std::string val;
            if (!requireValue(arg.c_str(), &val))
                return false;
            const std::vector<std::string> items = SplitCsvOrSpace(val);
            if (items.empty()) {
                *errorMsg = "Empty --ext list.";
                return false;
            }
            cfg->scanExtensions.clear();
            for (std::string ext : items) {
                ext = ToLowerAscii(std::move(ext));
                if (!ext.empty() && ext.front() != '.')
                    ext.insert(ext.begin(), '.');
                cfg->scanExtensions.push_back(std::move(ext));
            }
        }
        else if (!arg.empty() && arg.front() == '-') {
            *errorMsg = "Unknown option: " + arg;
            return false;
        }
        else {
            cfg->positionalArgs.push_back(arg);
        }
    }

    return true;
}

static bool CollectTasks(
    const CliConfig& cfg,
    std::vector<ScreenshotTask>* outTasks,
    size_t* outSkippedCount,
    std::string* errorMsg)
{
    std::vector<ScreenshotTask> rawTasks;

    if (!cfg.listFilePath.empty()) {
        std::ifstream ifs(PathFromUtf8(cfg.listFilePath), std::ios::binary);
        if (!ifs) {
            *errorMsg = "Could not open task list file: " + cfg.listFilePath;
            return false;
        }

        const std::filesystem::path outBaseDir = PathFromUtf8(cfg.outputDir);
        std::string line;
        size_t lineNumber = 0;
        while (std::getline(ifs, line)) {
            ++lineNumber;
            if (lineNumber == 1 && line.size() >= 3 &&
                static_cast<unsigned char>(line[0]) == 0xEF &&
                static_cast<unsigned char>(line[1]) == 0xBB &&
                static_cast<unsigned char>(line[2]) == 0xBF) {
                line.erase(0, 3);
            }
            if (!line.empty() && line.back() == '\r')
                line.pop_back();

            const std::string trimmed = Trim(line);
            if (trimmed.empty() || trimmed.front() == '#')
                continue;

            const size_t tabPos = line.find('\t');
            if (tabPos != std::string::npos) {
                const std::string inPath = Trim(std::string_view(line).substr(0, tabPos));
                const std::string outPath = Trim(std::string_view(line).substr(tabPos + 1));
                if (inPath.empty() || outPath.empty()) {
                    *errorMsg = "Invalid task at line " + std::to_string(lineNumber) + " in " + cfg.listFilePath;
                    return false;
                }
                rawTasks.push_back({ inPath, outPath });
            }
            else {
                const std::filesystem::path inFsPath = PathFromUtf8(trimmed);
                std::filesystem::path outFsPath;
                if (!outBaseDir.empty()) {
                    outFsPath = outBaseDir / (inFsPath.stem().native() + L"." + PathFromUtf8(cfg.outputFormat).native());
                }
                else {
                    outFsPath = inFsPath;
                    outFsPath.replace_extension(PathFromUtf8(cfg.outputFormat));
                }
                rawTasks.push_back({ trimmed, PathToUtf8(outFsPath) });
            }
        }
    }

    if (!cfg.positionalArgs.empty()) {
        if (cfg.positionalArgs.size() == 2 && cfg.outputDir.empty()) {
            const std::filesystem::path p0 = PathFromUtf8(cfg.positionalArgs[0]);
            const std::filesystem::path p1 = PathFromUtf8(cfg.positionalArgs[1]);
            std::error_code ec;
            if (std::filesystem::is_directory(p0, ec)) {
                std::unordered_set<std::string> allowedExts(
                    cfg.scanExtensions.begin(),
                    cfg.scanExtensions.end());

                std::vector<std::filesystem::path> discoveredFiles;
                for (const auto& entry : std::filesystem::recursive_directory_iterator(p0, ec)) {
                    if (!entry.is_regular_file(ec))
                        continue;
                    const std::string ext = ToLowerAscii(PathToUtf8(entry.path().extension()));
                    if (allowedExts.contains(ext))
                        discoveredFiles.push_back(entry.path());
                }
                std::sort(discoveredFiles.begin(), discoveredFiles.end());

                const std::filesystem::path formatExt = PathFromUtf8("." + cfg.outputFormat);
                for (const std::filesystem::path& file : discoveredFiles) {
                    std::filesystem::path rel = std::filesystem::relative(file, p0, ec);
                    if (ec || rel.empty())
                        rel = file.filename();
                    rel.replace_extension(formatExt);
                    rawTasks.push_back({ PathToUtf8(file), PathToUtf8(p1 / rel) });
                }
            }
            else {
                rawTasks.push_back({ cfg.positionalArgs[0], cfg.positionalArgs[1] });
            }
        }
        else if (!cfg.outputDir.empty()) {
            const std::filesystem::path outBaseDir = PathFromUtf8(cfg.outputDir);
            const std::filesystem::path formatExt = PathFromUtf8("." + cfg.outputFormat);
            for (const std::string& inputArg : cfg.positionalArgs) {
                const std::filesystem::path inFsPath = PathFromUtf8(inputArg);
                std::error_code ec;
                if (std::filesystem::is_directory(inFsPath, ec)) {
                    std::unordered_set<std::string> allowedExts(
                        cfg.scanExtensions.begin(),
                        cfg.scanExtensions.end());
                    std::vector<std::filesystem::path> discoveredFiles;
                    for (const auto& entry : std::filesystem::recursive_directory_iterator(inFsPath, ec)) {
                        if (!entry.is_regular_file(ec))
                            continue;
                        const std::string ext = ToLowerAscii(PathToUtf8(entry.path().extension()));
                        if (allowedExts.contains(ext))
                            discoveredFiles.push_back(entry.path());
                    }
                    std::sort(discoveredFiles.begin(), discoveredFiles.end());
                    for (const std::filesystem::path& file : discoveredFiles) {
                        std::filesystem::path rel = std::filesystem::relative(file, inFsPath, ec);
                        if (ec || rel.empty())
                            rel = file.filename();
                        rel.replace_extension(formatExt);
                        rawTasks.push_back({ PathToUtf8(file), PathToUtf8(outBaseDir / rel) });
                    }
                }
                else {
                    std::filesystem::path outFile = outBaseDir / inFsPath.filename();
                    outFile.replace_extension(formatExt);
                    rawTasks.push_back({ inputArg, PathToUtf8(outFile) });
                }
            }
        }
        else {
            *errorMsg = "Expected <input> <output>, or specify -o <outputDir> for multiple inputs.";
            return false;
        }
    }

    if (rawTasks.empty() && cfg.listFilePath.empty() && cfg.positionalArgs.empty()) {
        *errorMsg = "No input specified. Run with --help for usage.";
        return false;
    }

    size_t skipped = 0;
    outTasks->clear();
    outTasks->reserve(rawTasks.size());
    for (ScreenshotTask& task : rawTasks) {
        if (cfg.skipExisting) {
            std::error_code ec;
            if (std::filesystem::exists(PathFromUtf8(task.outputImagePath), ec)) {
                ++skipped;
                continue;
            }
        }
        outTasks->push_back(std::move(task));
    }

    if (outSkippedCount)
        *outSkippedCount = skipped;
    return true;
}

static int RunInProcessBatch(
    const std::vector<ScreenshotTask>& tasks,
    const RenderOptions& renderOptions,
    const CoordinatorOptions& coordinatorOptions,
    size_t skippedCount)
{
    ScreenshotRenderer renderer;
    std::string initError;
    if (!renderer.Initialize(renderOptions, &initError)) {
        std::cerr << "Initialization failed: " << initError << "\n";
        return 1;
    }

    size_t succeeded = 0;
    size_t failed = 0;
    for (size_t i = 0; i < tasks.size(); ++i) {
        std::string err;
        if (renderer.Render(tasks[i].usdFilePath, tasks[i].outputImagePath, &err)) {
            ++succeeded;
            if (!coordinatorOptions.quiet) {
                std::cout << "[" << (i + 1) << "/" << tasks.size() << "] OK   "
                          << tasks[i].usdFilePath << " -> " << tasks[i].outputImagePath << "\n";
            }
        }
        else {
            ++failed;
            std::cerr << "[" << (i + 1) << "/" << tasks.size() << "] FAIL "
                      << tasks[i].usdFilePath << " -> " << err << "\n";
        }
    }

    if (!coordinatorOptions.quiet && (tasks.size() > 1 || skippedCount > 0)) {
        std::cout << "Completed: " << succeeded << " succeeded, " << failed << " failed";
        if (skippedCount > 0)
            std::cout << ", " << skippedCount << " skipped";
        std::cout << ".\n";
    }

    return failed == 0 ? 0 : 1;
}

int wmain(int argc, wchar_t* argv[])
{
    std::vector<std::string> utf8Args;
    utf8Args.reserve(static_cast<size_t>(argc));
    for (int i = 0; i < argc; ++i)
        utf8Args.push_back(WideToUtf8(argv[i]));

    CliConfig cfg;
    std::string parseError;
    if (!ParseCommandLine(utf8Args, &cfg, &parseError)) {
        SetConsoleOutputCP(CP_UTF8);
        std::cerr << "Error: " << parseError << "\n\n";
        PrintUsage();
        return 2;
    }

    if (cfg.workerMode)
        return RunWorkerMode(cfg.render);

    SetConsoleOutputCP(CP_UTF8);

    if (cfg.showHelp || argc <= 1) {
        PrintUsage();
        return cfg.showHelp ? 0 : 2;
    }

    std::vector<ScreenshotTask> tasks;
    size_t skippedCount = 0;
    std::string collectError;
    if (!CollectTasks(cfg, &tasks, &skippedCount, &collectError)) {
        std::cerr << "Error: " << collectError << "\n";
        return 2;
    }

    if (tasks.empty()) {
        if (!cfg.coordinator.quiet) {
            std::cout << "No tasks to process";
            if (skippedCount > 0)
                std::cout << " (" << skippedCount << " skipped)";
            std::cout << ".\n";
        }
        return 0;
    }

    const auto startTime = std::chrono::steady_clock::now();

    if (cfg.inProcessMode)
        return RunInProcessBatch(tasks, cfg.render, cfg.coordinator, skippedCount);

    BatchSummary summary{};
    const bool allSucceeded = RunCoordinatorMode(tasks, cfg.render, cfg.coordinator, &summary);
    summary.skippedCount = skippedCount;

    const auto elapsedSec = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - startTime).count();

    if (!cfg.coordinator.quiet && (summary.totalCount > 1 || summary.skippedCount > 0)) {
        std::cout << "Completed in " << std::fixed << std::setprecision(2) << elapsedSec << "s: "
                  << summary.succeededCount << " succeeded, "
                  << summary.failedCount << " failed";
        if (summary.skippedCount > 0)
            std::cout << ", " << summary.skippedCount << " skipped";
        std::cout << ".\n";
    }

    return allSucceeded ? 0 : 1;
}
