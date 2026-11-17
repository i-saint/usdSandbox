#pragma once

#include "screenshotRenderer.h"

#include <cstddef>
#include <string>
#include <vector>

struct ScreenshotTask
{
    std::string usdFilePath;
    std::string outputImagePath;
};

struct CoordinatorOptions
{
    unsigned int workerCount = 0;
    bool verbose = false;
    bool quiet = false;
};

struct BatchSummary
{
    size_t totalCount = 0;
    size_t succeededCount = 0;
    size_t failedCount = 0;
    size_t skippedCount = 0;
};

int RunWorkerMode(const RenderOptions& renderOptions);

bool RunCoordinatorMode(
    const std::vector<ScreenshotTask>& tasks,
    const RenderOptions& renderOptions,
    const CoordinatorOptions& coordinatorOptions,
    BatchSummary* outSummary = nullptr);
