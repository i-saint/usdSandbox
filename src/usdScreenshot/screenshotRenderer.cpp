#include "screenshotRenderer.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#pragma warning(push, 0)
#pragma warning(disable: 4244 4273 4305 4996)
#include <pxr/pxr.h>
#include <pxr/base/gf/bbox3d.h>
#include <pxr/base/gf/camera.h>
#include <pxr/base/gf/math.h>
#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/range1f.h>
#include <pxr/base/gf/range3d.h>
#include <pxr/base/gf/rotation.h>
#include <pxr/base/gf/vec3d.h>
#include <pxr/base/plug/registry.h>
#include <pxr/base/tf/diagnosticMgr.h>
#include <pxr/base/tf/errorMark.h>
#include <pxr/base/tf/token.h>
#include <pxr/base/work/threadLimits.h>
#include <pxr/imaging/cameraUtil/conformWindow.h>
#include <pxr/imaging/garch/glApi.h>
#include <pxr/usd/ar/resolver.h>
#include <pxr/usd/ar/resolverContext.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/sdf/path.h>
#include <pxr/usd/usd/editContext.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/bboxCache.h>
#include <pxr/usd/usdGeom/camera.h>
#include <pxr/usd/usdGeom/metrics.h>
#include <pxr/usd/usdGeom/tokens.h>
#include <pxr/usdImaging/usdAppUtils/frameRecorder.h>
#include <pxr/usdImaging/usdImagingGL/engine.h>
#pragma warning(pop)

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <limits>
#include <sstream>

PXR_NAMESPACE_USING_DIRECTIVE

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

static std::filesystem::path GetExecutableDirectory()
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

static TfToken ResolveRendererPluginId(const std::string& requestedName)
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

static std::string CollectErrors(const TfErrorMark& mark, const std::string& fallbackMessage)
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

static GfCamera ComputeAutoFrameCamera(
    const UsdStageRefPtr& stage,
    UsdTimeCode timeCode,
    const TfTokenVector& includedPurposes,
    size_t imageWidth,
    size_t imageHeight)
{
    TfTokenVector bboxPurposes = { UsdGeomTokens->default_ };
    for (const TfToken& p : includedPurposes) {
        if (std::find(bboxPurposes.begin(), bboxPurposes.end(), p) == bboxPurposes.end())
            bboxPurposes.push_back(p);
    }

    UsdGeomBBoxCache bboxCache(timeCode, bboxPurposes, /*useExtentsHint=*/true);
    GfBBox3d bbox = bboxCache.ComputeWorldBound(stage->GetPseudoRoot());
    if (bbox.GetRange().IsEmpty()) {
        UsdGeomBBoxCache fallbackCache(timeCode, bboxPurposes, /*useExtentsHint=*/false);
        bbox = fallbackCache.ComputeWorldBound(stage->GetPseudoRoot());
    }

    GfVec3d center = bbox.ComputeCentroid();
    GfRange3d body = bbox.ComputeAlignedRange();
    if (body.IsEmpty()) {
        body = GfRange3d(GfVec3d(-10.0, -10.0, -10.0), GfVec3d(10.0, 10.0, 10.0));
        center = GfVec3d(0.0, 0.0, 0.0);
    }

    const TfToken upAxis = UsdGeomGetStageUpAxis(stage);
    const bool isZUp = (upAxis == UsdGeomTokens->z);

    const double aspectRatio = static_cast<double>(imageWidth) / static_cast<double>(imageHeight);
    GfCamera gfCamera;
    CameraUtilConformWindow(&gfCamera, CameraUtilFit, aspectRatio);

    float hAp = gfCamera.GetHorizontalAperture();
    const float vAp = gfCamera.GetVerticalAperture();
    for (int i = 0; i < 8 && vAp > 0.0f && hAp > 0.0f; ++i) {
        const size_t computedHeight = std::max<size_t>(
            static_cast<size_t>(static_cast<float>(imageWidth) / (hAp / vAp)), 1u);
        if (computedHeight < imageHeight)
            hAp = std::nextafter(hAp, 0.0f);
        else if (computedHeight > imageHeight)
            hAp = std::nextafter(hAp, std::numeric_limits<float>::infinity());
        else
            break;
    }
    gfCamera.SetHorizontalAperture(hAp);

    const GfVec3d viewDir = GfVec3d(0.5, 0.7, 1.0).GetNormalized();
    GfMatrix4d viewRot(1.0);
    viewRot.SetLookAt(GfVec3d(0.0, 0.0, 0.0), -viewDir, GfVec3d(0.0, 1.0, 0.0));

    const GfMatrix4d worldToViewAligned =
        (isZUp ? GfMatrix4d(1.0).SetRotate(GfRotation(GfVec3d::XAxis(), -90.0)) : GfMatrix4d(1.0)) * viewRot;

    const double fovH = GfDegreesToRadians(static_cast<double>(gfCamera.GetFieldOfView(GfCamera::FOVHorizontal)));
    const double fovV = GfDegreesToRadians(static_cast<double>(gfCamera.GetFieldOfView(GfCamera::FOVVertical)));
    const double tanHalfH = std::tan(fovH * 0.5);
    const double tanHalfV = std::tan(fovV * 0.5);
    constexpr double kMargin = 1.05;

    double distance = 0.0;
    if (tanHalfH > 1e-6 && tanHalfV > 1e-6) {
        for (int i = 0; i < 8; ++i) {
            const GfVec3d rel = body.GetCorner(i) - center;
            const GfVec3d v = worldToViewAligned.TransformDir(rel);
            distance = std::max(distance, v[2] + (std::abs(v[0]) * kMargin) / tanHalfH);
            distance = std::max(distance, v[2] + (std::abs(v[1]) * kMargin) / tanHalfV);
        }
    }
    if (distance <= 0.0) {
        const double metersPerUnit = UsdGeomGetStageMetersPerUnit(stage);
        distance = (metersPerUnit > 0.0) ? (1.0 / metersPerUnit) : 100.0;
    }

    const double radius = std::max(body.GetSize().GetLength() * 0.5, 1e-3);
    const float nearClip = static_cast<float>(std::max(std::max((distance - radius) * 0.1, distance * 1e-4), 1e-6));
    const float farClip = static_cast<float>(std::max((distance + radius) * 10.0, static_cast<double>(nearClip) * 1000.0));
    gfCamera.SetClippingRange(GfRange1f(nearClip, farClip));

    GfMatrix4d camTransform = viewRot.GetInverse() * GfMatrix4d(1.0).SetTranslate(viewDir * distance);
    if (isZUp)
        camTransform = camTransform * GfMatrix4d(1.0).SetRotate(GfRotation(GfVec3d::XAxis(), 90.0));
    camTransform = camTransform * GfMatrix4d(1.0).SetTranslate(center);
    gfCamera.SetTransform(camTransform);

    return gfCamera;
}

bool ScreenshotRenderer::Initialize(const RenderOptions& options, std::string* errorMessage)
{
    m_options = options;

    if (m_options.threadsPerWorker > 0)
        WorkSetConcurrencyLimit(m_options.threadsPerWorker);

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

    const size_t width = m_options.imageWidth > 0 ? m_options.imageWidth : 1;
    const size_t height = m_options.imageHeight > 0 ? m_options.imageHeight : 1;

    const GfCamera gfCamera = ComputeAutoFrameCamera(stage, timeCode, purposeTokens, width, height);

    UsdGeomCamera screenshotCamera;
    {
        UsdEditContext editContext(stage, stage->GetSessionLayer());
        screenshotCamera = UsdGeomCamera::Define(stage, SdfPath("/__ScreenshotCamera"));
        if (!screenshotCamera) {
            if (errorMessage)
                *errorMessage = CollectErrors(errorMark, "Failed to define /__ScreenshotCamera");
            errorMark.Clear();
            return false;
        }
        screenshotCamera.SetFromCamera(gfCamera, timeCode);
    }

    const TfToken rendererId = m_resolvedRendererId.empty()
        ? TfToken()
        : TfToken(m_resolvedRendererId);

    UsdAppUtilsFrameRecorder recorder(
        rendererId,
        m_options.gpuEnabled,
        m_options.enableUsdDrawModes);

    recorder.SetImageWidth(width);
    recorder.SetComplexity(m_options.complexity);
    recorder.SetCameraLightEnabled(m_options.cameraLightEnabled);
    recorder.SetColorCorrectionMode(TfToken(m_options.colorCorrectionMode));
    recorder.SetIncludedPurposes(purposeTokens);
    recorder.SetDomeLightVisibility(m_options.domeLightVisibility);
    recorder.SetPrimaryCameraPrimPath(screenshotCamera.GetPath());

    const bool recorded = recorder.Record(stage, screenshotCamera, timeCode, outputImagePath);
    if (!recorded) {
        if (errorMessage)
            *errorMessage = CollectErrors(errorMark, "Failed to record screenshot: " + usdFilePath);
        errorMark.Clear();
        return false;
    }

    errorMark.Clear();
    return true;
}
