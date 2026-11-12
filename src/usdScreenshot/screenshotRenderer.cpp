#include "screenshotRenderer.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#pragma warning(push, 0)
#pragma warning(disable: 4244 4273 4305 4996)
#include <pxr/pxr.h>
#include <pxr/base/plug/registry.h>
#include <pxr/base/tf/diagnosticMgr.h>
#include <pxr/base/tf/errorMark.h>
#include <pxr/base/tf/token.h>
#include <pxr/base/work/threadLimits.h>
#include <pxr/imaging/garch/glApi.h>
#include <pxr/usd/ar/resolver.h>
#include <pxr/usd/ar/resolverContext.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/sdf/path.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/camera.h>
#include <pxr/usd/usdRender/settings.h>
#include <pxr/usd/usdUtils/pipeline.h>
#include <pxr/usdImaging/usdAppUtils/camera.h>
#include <pxr/usdImaging/usdAppUtils/frameRecorder.h>
#include <pxr/usdImaging/usdImagingGL/engine.h>
#pragma warning(pop)

#include <filesystem>
#include <sstream>

PXR_NAMESPACE_USING_DIRECTIVE

namespace {

std::filesystem::path PathFromUtf8(const std::string& utf8)
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

std::string PathToUtf8(const std::filesystem::path& path)
{
    const std::wstring& wide = path.native();
    if (wide.empty())
        return {};
    const int utf8Len = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    if (utf8Len <= 0)
        return {};
    std::string utf8(static_cast<size_t>(utf8Len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), utf8.data(), utf8Len, nullptr, nullptr);
    return utf8;
}

std::filesystem::path GetExecutableDirectory()
{
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
        const DWORD len = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (len == 0)
            return std::filesystem::current_path();
        if (len < buffer.size() - 1) {
            buffer.resize(len);
            return std::filesystem::path(buffer).parent_path();
        }
        buffer.resize(buffer.size() * 2);
    }
}

TfToken ResolveRendererPluginId(const std::string& requestedName)
{
    if (requestedName.empty())
        return TfToken();

    const std::string normalized = (requestedName == "Storm") ? "GL" : requestedName;
    for (const TfToken& pluginId : UsdImagingGLEngine::GetRendererPlugins()) {
        if (pluginId.GetString() == requestedName ||
            pluginId.GetString() == normalized ||
            UsdImagingGLEngine::GetRendererDisplayName(pluginId) == requestedName ||
            UsdImagingGLEngine::GetRendererDisplayName(pluginId) == normalized) {
            return pluginId;
        }
    }
    return TfToken(requestedName);
}

std::string CollectErrors(const TfErrorMark& mark, const std::string& fallbackMessage)
{
    if (mark.IsClean())
        return fallbackMessage;

    std::ostringstream oss;
    bool first = true;
    for (const TfError& err : mark) {
        if (!first)
            oss << "; ";
        first = false;
        oss << err.GetCommentary();
    }
    const std::string combined = oss.str();
    return combined.empty() ? fallbackMessage : combined;
}

UsdGeomCamera ResolveCameraForStage(
    const UsdStageRefPtr& stage,
    const std::string& requestedCamera,
    const SdfPath& renderSettingsPath)
{
    UsdGeomCamera usdCamera;
    if (!requestedCamera.empty()) {
        const SdfPath camPath(requestedCamera);
        if (!camPath.IsEmpty())
            usdCamera = UsdAppUtilsGetCameraAtPath(stage, camPath);
    }

    if (!usdCamera && !renderSettingsPath.IsEmpty()) {
        if (const UsdRenderSettings rs = UsdRenderSettings(stage->GetPrimAtPath(renderSettingsPath))) {
            SdfPathVector targets;
            rs.GetCameraRel().GetTargets(&targets);
            if (!targets.empty())
                usdCamera = UsdGeomCamera(stage->GetPrimAtPath(targets.front()));
        }
    }

    if (!usdCamera) {
        const TfToken primaryCameraName = UsdUtilsGetPrimaryCameraName();
        if (!primaryCameraName.IsEmpty())
            usdCamera = UsdAppUtilsGetCameraAtPath(stage, SdfPath(primaryCameraName.GetString()));
    }

    return usdCamera;
}

} // namespace

void RegisterUsdPluginsFromExecutableDir()
{
    static bool registered = false;
    if (registered)
        return;
    registered = true;

    const std::filesystem::path exeDir = GetExecutableDirectory();
    std::vector<std::string> pluginPaths;

    auto addIfExists = [&pluginPaths](const std::filesystem::path& p) {
        std::error_code ec;
        if (std::filesystem::exists(p, ec))
            pluginPaths.push_back(PathToUtf8(p));
    };

    // Standard deployed USD plugins next to executable / usd_ms.dll
    addIfExists(exeDir / L"usd" / L"plugInfo.json");
    addIfExists(exeDir / L"plugInfo.json");
    addIfExists(exeDir / L"resources" / L"plugInfo.json");

    // Local plugins in sibling subdirectories (e.g. usdObjPlus/plugInfo.json)
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(exeDir, ec)) {
        if (!entry.is_directory(ec))
            continue;
        if (entry.path().filename() == L"usd")
            continue;
        addIfExists(entry.path() / L"plugInfo.json");
        addIfExists(entry.path() / L"resources" / L"plugInfo.json");
    }

    // Fallback to workspace externals/usd when running from _bin/<config>/
    const std::filesystem::path workspaceSdkRoot = exeDir.parent_path().parent_path() / L"externals" / L"usd";
    if (!std::filesystem::exists(exeDir / L"usd" / L"plugInfo.json", ec)) {
        addIfExists(workspaceSdkRoot / L"lib" / L"usd" / L"plugInfo.json");
        addIfExists(workspaceSdkRoot / L"plugin" / L"usd" / L"plugInfo.json");
    }

    if (!pluginPaths.empty())
        PlugRegistry::GetInstance().RegisterPlugins(pluginPaths);
}

bool ScreenshotRenderer::Initialize(const RenderOptions& options, std::string* errorMessage)
{
    m_options = options;

    if (m_options.threadsPerWorker > 0)
        WorkSetConcurrencyLimit(m_options.threadsPerWorker);

    RegisterUsdPluginsFromExecutableDir();

    if (m_options.gpuEnabled) {
        if (!m_glContext.Initialize(errorMessage))
            return false;
        GarchGLApiLoad();
    }

    if (!m_options.rendererPlugin.empty()) {
        const TfToken resolved = ResolveRendererPluginId(m_options.rendererPlugin);
        const TfTokenVector available = UsdImagingGLEngine::GetRendererPlugins();
        bool found = false;
        for (const TfToken& id : available) {
            if (id == resolved) {
                found = true;
                break;
            }
        }
        if (!found) {
            if (errorMessage)
                *errorMessage = "Unknown Hydra renderer plugin: " + m_options.rendererPlugin;
            return false;
        }
        m_resolvedRendererId = resolved.GetString();
    }

    m_initialized = true;
    return true;
}

bool ScreenshotRenderer::Render(
    const std::string& usdFilePath,
    const std::string& outputImagePath,
    std::string* errorMessage)
{
    if (!m_initialized) {
        if (errorMessage)
            *errorMessage = "ScreenshotRenderer is not initialized.";
        return false;
    }

    if (m_options.gpuEnabled && !m_glContext.MakeCurrent()) {
        if (errorMessage)
            *errorMessage = "Failed to activate WGL context.";
        return false;
    }

    const std::filesystem::path outFsPath = PathFromUtf8(outputImagePath);
    if (outFsPath.has_parent_path()) {
        std::error_code ec;
        std::filesystem::create_directories(outFsPath.parent_path(), ec);
        if (ec) {
            if (errorMessage)
                *errorMessage = "Failed to create output directory: " + PathToUtf8(outFsPath.parent_path());
            return false;
        }
    }

    TfErrorMark errorMark;

    SdfLayerRefPtr rootLayer = SdfLayer::FindOrOpen(usdFilePath);
    if (!rootLayer) {
        if (errorMessage)
            *errorMessage = CollectErrors(errorMark, "Could not open layer: " + usdFilePath);
        errorMark.Clear();
        return false;
    }

    SdfLayerRefPtr sessionLayer = SdfLayer::CreateAnonymous();
    ArResolverContext resolverContext = ArGetResolver().CreateDefaultContextForAsset(usdFilePath);
    UsdStageRefPtr stage = UsdStage::Open(rootLayer, sessionLayer, resolverContext);
    if (!stage) {
        if (errorMessage)
            *errorMessage = CollectErrors(errorMark, "Could not open USD stage: " + usdFilePath);
        errorMark.Clear();
        return false;
    }

    SdfPath renderSettingsPath;
    std::string rsPathStr;
    if (stage->GetMetadata(TfToken("renderSettingsPrimPath"), &rsPathStr) && !rsPathStr.empty())
        renderSettingsPath = SdfPath(rsPathStr);

    UsdGeomCamera usdCamera = ResolveCameraForStage(stage, m_options.camera, renderSettingsPath);

    UsdTimeCode timeCode = UsdTimeCode::Default();
    if (m_options.timeCode.has_value()) {
        timeCode = UsdTimeCode(*m_options.timeCode);
    }
    else if (m_options.earliestTime && stage->HasAuthoredTimeCodeRange()) {
        timeCode = UsdTimeCode(stage->GetStartTimeCode());
    }

    TfTokenVector purposeTokens;
    purposeTokens.reserve(m_options.purposes.size());
    for (const std::string& p : m_options.purposes) {
        if (!p.empty())
            purposeTokens.emplace_back(p);
    }

    const TfToken rendererId = m_resolvedRendererId.empty()
        ? TfToken()
        : TfToken(m_resolvedRendererId);

    UsdAppUtilsFrameRecorder recorder(
        rendererId,
        m_options.gpuEnabled,
        m_options.enableUsdDrawModes);

    if (!renderSettingsPath.IsEmpty())
        recorder.SetActiveRenderSettingsPrimPath(renderSettingsPath);

    recorder.SetImageWidth(m_options.imageWidth > 0 ? m_options.imageWidth : 1);
    recorder.SetComplexity(m_options.complexity);
    recorder.SetCameraLightEnabled(m_options.cameraLightEnabled);
    recorder.SetColorCorrectionMode(TfToken(m_options.colorCorrectionMode));
    recorder.SetIncludedPurposes(purposeTokens);
    recorder.SetDomeLightVisibility(m_options.domeLightVisibility);
    if (usdCamera)
        recorder.SetPrimaryCameraPrimPath(usdCamera.GetPath());

    const bool recorded = recorder.Record(stage, usdCamera, timeCode, outputImagePath);
    if (!recorded) {
        if (errorMessage)
            *errorMessage = CollectErrors(errorMark, "Failed to record screenshot: " + usdFilePath);
        errorMark.Clear();
        return false;
    }

    errorMark.Clear();
    return true;
}
