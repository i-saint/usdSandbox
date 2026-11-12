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
#include <pxr/imaging/hio/image.h>
#include <pxr/imaging/hio/types.h>
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
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <sstream>
#include <vector>

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

static float SRGBToLinear(float s)
{
    s = std::clamp(s, 0.0f, 1.0f);
    return (s <= 0.04045f) ? (s / 12.92f) : std::pow((s + 0.055f) / 1.055f, 2.4f);
}

static float LinearToSRGB(float l)
{
    l = std::clamp(l, 0.0f, 1.0f);
    return (l <= 0.0031308f) ? (l * 12.92f) : (1.055f * std::pow(l, 1.0f / 2.4f) - 0.055f);
}

static const std::array<float, 256>& GetSRGBToLinearLUT()
{
    static const std::array<float, 256> lut = [] {
        std::array<float, 256> table{};
        for (size_t i = 0; i < 256; ++i)
            table[i] = SRGBToLinear(static_cast<float>(i) * (1.0f / 255.0f));
        return table;
    }();
    return lut;
}

static std::string ToLowerAscii(std::string s)
{
    for (char& ch : s)
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return s;
}

static bool CompositeBackgroundAndWrite(
    const std::string& recordedImagePath,
    const std::string& outputImagePath,
    const std::string& outputExtLower,
    const float bgColor[3],
    bool isSRGB)
{
    HioImageSharedPtr reader = HioImage::OpenForReading(
        recordedImagePath, 0, 0, HioImage::SourceColorSpace::Raw);
    if (!reader)
        return false;

    const int width = reader->GetWidth();
    const int height = reader->GetHeight();
    if (width <= 0 || height <= 0)
        return false;

    const size_t pixelCount = static_cast<size_t>(width) * static_cast<size_t>(height);

    if (outputExtLower == ".exr" || outputExtLower == ".hdr") {
        std::vector<float> pixels(pixelCount * 4);
        HioImage::StorageSpec readSpec;
        readSpec.width = width;
        readSpec.height = height;
        readSpec.depth = 1;
        readSpec.flipped = false;
        readSpec.format = HioFormatFloat32Vec4;
        readSpec.data = pixels.data();
        if (!reader->Read(readSpec))
            return false;

        for (size_t i = 0; i < pixelCount; ++i) {
            float* px = &pixels[i * 4];
            const float invA = 1.0f - std::clamp(px[3], 0.0f, 1.0f);
            px[0] += invA * bgColor[0];
            px[1] += invA * bgColor[1];
            px[2] += invA * bgColor[2];
            px[3] = 1.0f;
        }

        HioImageSharedPtr writer = HioImage::OpenForWriting(outputImagePath);
        if (!writer)
            return false;

        HioImage::StorageSpec writeSpec = readSpec;
        return writer->Write(writeSpec);
    }

    std::vector<uint8_t> pixels(pixelCount * 4);
    HioImage::StorageSpec readSpec;
    readSpec.width = width;
    readSpec.height = height;
    readSpec.depth = 1;
    readSpec.flipped = false;
    readSpec.format = HioFormatUNorm8Vec4;
    readSpec.data = pixels.data();
    if (!reader->Read(readSpec))
        return false;

    const uint8_t bgByte[3] = {
        static_cast<uint8_t>(std::lround(std::clamp(bgColor[0], 0.0f, 1.0f) * 255.0f)),
        static_cast<uint8_t>(std::lround(std::clamp(bgColor[1], 0.0f, 1.0f) * 255.0f)),
        static_cast<uint8_t>(std::lround(std::clamp(bgColor[2], 0.0f, 1.0f) * 255.0f)),
    };

    if (isSRGB) {
        const auto& srgbToLinear = GetSRGBToLinearLUT();
        const float bgLinear[3] = {
            SRGBToLinear(bgColor[0]),
            SRGBToLinear(bgColor[1]),
            SRGBToLinear(bgColor[2]),
        };

        for (size_t i = 0; i < pixelCount; ++i) {
            uint8_t* px = &pixels[i * 4];
            const uint8_t a = px[3];
            if (a == 255)
                continue;
            if (a == 0) {
                px[0] = bgByte[0];
                px[1] = bgByte[1];
                px[2] = bgByte[2];
                px[3] = 255;
                continue;
            }
            const float invA = 1.0f - (static_cast<float>(a) * (1.0f / 255.0f));
            for (int c = 0; c < 3; ++c) {
                const float lin = srgbToLinear[px[c]] + invA * bgLinear[c];
                px[c] = static_cast<uint8_t>(std::lround(LinearToSRGB(lin) * 255.0f));
            }
            px[3] = 255;
        }
    }
    else {
        for (size_t i = 0; i < pixelCount; ++i) {
            uint8_t* px = &pixels[i * 4];
            const uint8_t a = px[3];
            if (a == 255)
                continue;
            if (a == 0) {
                px[0] = bgByte[0];
                px[1] = bgByte[1];
                px[2] = bgByte[2];
                px[3] = 255;
                continue;
            }
            const float invA = 1.0f - (static_cast<float>(a) * (1.0f / 255.0f));
            for (int c = 0; c < 3; ++c) {
                const float v = std::clamp(
                    (static_cast<float>(px[c]) * (1.0f / 255.0f)) + invA * bgColor[c], 0.0f, 1.0f);
                px[c] = static_cast<uint8_t>(std::lround(v * 255.0f));
            }
            px[3] = 255;
        }
    }

    HioImageSharedPtr writer = HioImage::OpenForWriting(outputImagePath);
    if (!writer)
        return false;

    if (outputExtLower == ".jpg" || outputExtLower == ".jpeg" || outputExtLower == ".bmp") {
        std::vector<uint8_t> rgb(pixelCount * 3);
        for (size_t i = 0; i < pixelCount; ++i) {
            rgb[i * 3 + 0] = pixels[i * 4 + 0];
            rgb[i * 3 + 1] = pixels[i * 4 + 1];
            rgb[i * 3 + 2] = pixels[i * 4 + 2];
        }
        HioImage::StorageSpec writeSpec;
        writeSpec.width = width;
        writeSpec.height = height;
        writeSpec.depth = 1;
        writeSpec.flipped = false;
        writeSpec.format = HioFormatUNorm8Vec3;
        writeSpec.data = rgb.data();
        return writer->Write(writeSpec);
    }

    HioImage::StorageSpec writeSpec = readSpec;
    return writer->Write(writeSpec);
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

    const std::string outputExtLower = ToLowerAscii(PathToUtf8(outFsPath.extension()));
    const wchar_t* tempExt = (outputExtLower == ".exr" || outputExtLower == ".hdr") ? L".exr" : L".png";
    const std::filesystem::path tempFsPath = outFsPath.parent_path() /
        (outFsPath.stem().native() + L".__usdss_" + std::to_wstring(GetCurrentProcessId()) + tempExt);
    const std::string tempImagePath = PathToUtf8(tempFsPath);

    struct TempFileGuard
    {
        std::filesystem::path path;
        ~TempFileGuard()
        {
            if (!path.empty()) {
                std::error_code ec;
                std::filesystem::remove(path, ec);
            }
        }
    } tempGuard{ tempFsPath };

    const bool recorded = recorder.Record(stage, screenshotCamera, timeCode, tempImagePath);
    if (!recorded) {
        if (errorMessage)
            *errorMessage = CollectErrors(errorMark, "Failed to record screenshot: " + usdFilePath);
        errorMark.Clear();
        return false;
    }

    const bool isSRGB = (m_options.colorCorrectionMode == "sRGB");
    if (!CompositeBackgroundAndWrite(
            tempImagePath, outputImagePath, outputExtLower, m_options.backgroundColor, isSRGB)) {
        if (errorMessage)
            *errorMessage = CollectErrors(errorMark, "Failed to composite background: " + outputImagePath);
        errorMark.Clear();
        return false;
    }

    errorMark.Clear();
    return true;
}
