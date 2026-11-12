#pragma once

#include "wglContext.h"

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

struct RenderOptions
{
    size_t imageWidth = 512;
    size_t imageHeight = 512;
    float backgroundColor[3] = { 0.3f, 0.3f, 0.3f };
    float complexity = 1.0f;
    std::string colorCorrectionMode = "sRGB";
    std::string rendererPlugin;
    std::optional<double> timeCode;
    bool earliestTime = false;
    bool gpuEnabled = true;
    bool enableUsdDrawModes = true;
    bool cameraLightEnabled = true;
    bool domeLightVisibility = false;
    std::vector<std::string> purposes = { "render" };
    unsigned int threadsPerWorker = 0;
};

void RegisterUsdPluginsFromExecutableDir();

class ScreenshotRenderer
{
public:
    ScreenshotRenderer() = default;
    ~ScreenshotRenderer() = default;

    ScreenshotRenderer(const ScreenshotRenderer&) = delete;
    ScreenshotRenderer& operator=(const ScreenshotRenderer&) = delete;

    bool Initialize(const RenderOptions& options, std::string* errorMessage = nullptr);
    bool Render(
        const std::string& usdFilePath,
        const std::string& outputImagePath,
        std::string* errorMessage = nullptr);

private:
    RenderOptions m_options;
    OffscreenWglContext m_glContext;
    std::string m_resolvedRendererId;
    bool m_initialized = false;
};
