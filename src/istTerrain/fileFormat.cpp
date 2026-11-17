#include "pch.h"
#include "fileFormat.h"
#include "unorm.h"


PXR_NAMESPACE_OPEN_SCOPE

TF_DEFINE_PUBLIC_TOKENS(
    UsdTerrainFileFormatTokens,
    USD_TERRAIN_FILE_FORMAT_TOKENS);

TF_REGISTRY_FUNCTION(TfType)
{
    SDF_DEFINE_FILE_FORMAT(UsdTerrainFileFormat, SdfFileFormat);
}


#pragma region Utils
template<class T>
inline std::string ToString(const T& value)
{
    if constexpr (std::is_same_v<T, std::string>) {
        return value;
    }
    else {
        return std::to_string(value);
    }
}

template<class T>
inline T Parse(std::string_view str)
{
    if constexpr (std::is_same_v<T, std::string>) {
        return std::string(str);
    }
    else if constexpr (std::is_arithmetic_v<T>) {
        T value;
        std::from_chars(str.data(), str.data() + str.size(), value);
        return value;
    }
    else {
        static_assert(std::is_same_v<T, void>, "Unsupported type");
    }
}

template<class T>
inline std::optional<T> Lookup(const VtDictionary& dict, const TfToken& key)
{
    if (auto it = dict.find(key); it != dict.end() && it->second.IsHolding<T>()) {
        return it->second.UncheckedGet<T>();
    }
    return std::nullopt;
}

template<class T>
inline std::optional<T> Lookup(const SdfFileFormat::FileFormatArguments& args, const TfToken& key)
{
    if (auto it = args.find(key); it != args.end()) {
        return Parse<T>(it->second);
    }
    return std::nullopt;
}

template<class To, class From>
struct DefaultConverter
{
    To operator()(const From& value) const
    {
        return static_cast<To>(value);
    }
};

template<class To, class From, class Converter = DefaultConverter<To, From>>
inline VtArray<To> ConvertArray(std::span<const From> array, Converter&& converter = Converter())
{
    VtArray<To> result(array.size());
    To* dst = result.data();
    for (size_t i = 0; i < array.size(); ++i) {
        dst[i] = converter(array[i]);
    }
    return result;
}

static bool ReadFileToString(const std::string& path, std::string& dst)
{
    HANDLE hFile = ::CreateFileA(
        path.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (hFile == INVALID_HANDLE_VALUE) {
        return false;
    }

    LARGE_INTEGER fileSize{};
    if (!::GetFileSizeEx(hFile, &fileSize)) {
        ::CloseHandle(hFile);
        return false;
    }

    dst.resize(static_cast<size_t>(fileSize.QuadPart));
    constexpr DWORD kChunkSize = 0x80000000u; // 2GB
    size_t totalRead = 0;
    while (totalRead < dst.size()) {
        DWORD toRead = static_cast<DWORD>(std::min<size_t>(dst.size() - totalRead, kChunkSize));
        DWORD bytesRead = 0;
        if (!::ReadFile(hFile, dst.data() + totalRead, toRead, &bytesRead, nullptr) || bytesRead == 0) {
            ::CloseHandle(hFile);
            return false;
        }
        totalRead += bytesRead;
    }
    dst.resize(totalRead);

    ::CloseHandle(hFile);
    return true;
}
#pragma endregion Utils


#pragma region UsdTerrainGenerator

class UsdTerrainGenerator
{
public:
    class Params
    {
    public:
#define Body(name, type, defaultValue) type name = defaultValue;
        USD_TERRAIN_PARAMS_EACH(Body);
#undef Body

        void Parse(const SdfFileFormat::FileFormatArguments& args);
    };

    class MeshData
    {
    public:
        VtArray<int> counts;
        VtArray<int> indices;
        VtArray<GfVec3f> vertices;
        VtArray<GfVec3f> normals;
        VtArray<GfVec2f> uvs;

        bool Generate(const HioImage::StorageSpec& image, const Params& params, int xdiv, int ydiv);

    private:
        template<class SamplerT>
        bool Pass1(SamplerT sampler, const Params& params, int xdiv, int ydiv);
        bool Pass2(const Params& params, int xdiv, int ydiv);
    };

    UsdTerrainGenerator(SdfLayer* layer);
    bool Generate();

private:
    SdfLayer* _layer = nullptr;
    Params _params;
};



void UsdTerrainGenerator::Params::Parse(const SdfFileFormat::FileFormatArguments& args)
{
#define Body(name, type, _) if (auto value = Lookup<type>(args, UsdTerrainFileFormatTokens->name)) { name = *value; }
    USD_TERRAIN_PARAMS_EACH(Body);
#undef Body

    MaxLodLevel = std::max(1, MaxLodLevel);
    DefaultLodLevel = std::clamp(DefaultLodLevel, 0, MaxLodLevel - 1);
    LodShift = std::clamp(LodShift, 1, 8);
}

template<class T>
class PointSampler
{
public:
    using ValueType = T;

    template<class U>
    static PointSampler<T> Create(const HioImage::StorageSpec& image)
    {
        return PointSampler<T>(ConvertArray<T>(std::span<const U>(static_cast<const U*>(image.data), image.width * image.height)), image.width, image.height);
    }

    template<class U>
    PointSampler(VtArray<T> image, int width, int height)
        : _image(std::move(image)), _width(width), _height(height) {}

    T operator()(float u, float v) const
    {
        int x = std::clamp(static_cast<int>(u * _width), 0, _width - 1);
        int y = std::clamp(static_cast<int>(v * _height), 0, _height - 1);
        return _image[y * _width + x];
    }

private:
    const VtArray<T> _image;
    int _width;
    int _height;
};

template<class T>
class BilinearSampler
{
public:
    using ValueType = T;

    template<class U>
    static BilinearSampler<T> Create(const HioImage::StorageSpec& image)
    {
        return BilinearSampler<T>(ConvertArray<T>(std::span<const U>(static_cast<const U*>(image.data), image.width* image.height)), image.width, image.height);
    }

    BilinearSampler(VtArray<T> image, int width, int height)
        : _image(std::move(image)), _width(width), _height(height) {}

    T operator()(float u, float v) const
    {
        float fx = u * (_width - 1);
        float fy = v * (_height - 1);

        int x0 = static_cast<int>(fx);
        int y0 = static_cast<int>(fy);
        int x1 = std::min(x0 + 1, _width - 1);
        int y1 = std::min(y0 + 1, _height - 1);

        float tx = fx - x0;
        float ty = fy - y0;
        T h00 = _image[y0 * _width + x0];
        T h10 = _image[y0 * _width + x1];
        T h01 = _image[y1 * _width + x0];
        T h11 = _image[y1 * _width + x1];
        T h0 = h00 * (1.0f - tx) + h10 * tx;
        T h1 = h01 * (1.0f - tx) + h11 * tx;
        T h = h0 * (1.0f - ty) + h1 * ty;
        return h;
    }

private:
    const VtArray<T> _image;
    int _width;
    int _height;
};

bool UsdTerrainGenerator::MeshData::Generate(const HioImage::StorageSpec& image, const Params& params, int xdiv, int ydiv)
{
    bool success = false;

    if (image.format == HioFormatUNorm8 || image.format == HioFormatUNorm8srgb) {
        success = Pass1(BilinearSampler<float>::Create<Unorm8>(image), params, xdiv, ydiv);
    }
    else if (image.format == HioFormatSNorm8) {
        success = Pass1(BilinearSampler<float>::Create<Snorm8>(image), params, xdiv, ydiv);
    }
    else if (image.format == HioFormatFloat16) {
        success = Pass1(BilinearSampler<float>::Create<GfHalf>(image), params, xdiv, ydiv);
    }
    else if (image.format == HioFormatFloat32) {
        success = Pass1(BilinearSampler<float>::Create<float>(image), params, xdiv, ydiv);
    }

    else if (image.format == HioFormatUNorm8Vec3 || image.format == HioFormatUNorm8Vec3srgb) {
        success = Pass1(BilinearSampler<GfVec3f>::Create<Vec3Unorm8>(image), params, xdiv, ydiv);
    }
    else if (image.format == HioFormatSNorm8Vec3) {
        success = Pass1(BilinearSampler<GfVec3f>::Create<Vec3Snorm8>(image), params, xdiv, ydiv);
    }
    else if (image.format == HioFormatFloat16Vec3) {
        success = Pass1(BilinearSampler<GfVec3f>::Create<GfVec3h>(image), params, xdiv, ydiv);
    }
    else if (image.format == HioFormatFloat32Vec3) {
        success = Pass1(BilinearSampler<GfVec3f>::Create<GfVec3f>(image), params, xdiv, ydiv);
    }

    else {
        TF_WARN("Unsupported image format: %s", TfEnum::GetName(image.format).c_str());
        success = false;
    }

    if (success) {
        success = Pass2(params, xdiv, ydiv);
    }
    return success;
}

template<class SamplerT>
bool UsdTerrainGenerator::MeshData::Pass1(SamplerT sampler, const Params& params, int xdiv, int ydiv)
{
    using ValueType = typename SamplerT::ValueType;

    int numVertices = xdiv * ydiv;
    vertices.resize(numVertices);
    uvs.resize(numVertices);

    float dx = params.XSize / (xdiv - 1);
    float dy = params.YSize / (ydiv - 1);
    float du = 1.0f / (xdiv - 1);
    float dv = 1.0f / (ydiv - 1);

    float zRange = params.MaxHeight;
    GfVec3f xyzRange(params.MaxXRange, params.MaxYRange, params.MaxHeight);
    for (int j = 0; j < ydiv; ++j) {
        for (int i = 0; i < xdiv; ++i) {
            float x = dx * i;
            float y = dy * j;
            float u = du * i;
            float v = dv * j;
            if constexpr (VecSize<ValueType> == 0) {
                auto z = sampler(u, v) * zRange;
                vertices[j * xdiv + i] = GfVec3f(x, y, z);
            }
            else if constexpr (VecSize<ValueType> == 3) {
                auto dir = GfCompMult(sampler(u, v), xyzRange);
                vertices[j * xdiv + i] = GfVec3f(x, y, 0.0f) + dir;
            }
            else {
                static_assert(VecSize<ValueType> == 0, "Unsupported sampler value type");
            }
            uvs[j * xdiv + i] = GfVec2f(u, v);
        }
    }
    return true;
}

bool UsdTerrainGenerator::MeshData::Pass2(const Params& params, int xdiv, int ydiv)
{
    int numVertices = xdiv * ydiv;
    int numQuads = (xdiv - 1) * (ydiv - 1);
    int numIndices = numQuads * 6;
    counts.resize(numQuads * 2, 3);
    indices.resize(numIndices);
    normals.resize(numVertices);

    for (int j = 0; j < ydiv; ++j) {
        for (int i = 0; i < xdiv; ++i) {
            GfVec3f normal(0.0f, 0.0f, 0.0f);
            if (i > 0 && j > 0) {
                GfVec3f v0 = vertices[j * xdiv + i] - vertices[(j - 1) * xdiv + i - 1];
                GfVec3f v1 = vertices[(j - 1) * xdiv + i] - vertices[(j - 1) * xdiv + i - 1];
                normal += GfCross(v1, v0);
            }
            if (i < xdiv - 1 && j > 0) {
                GfVec3f v0 = vertices[j * xdiv + i] - vertices[(j - 1) * xdiv + i + 1];
                GfVec3f v1 = vertices[(j - 1) * xdiv + i] - vertices[(j - 1) * xdiv + i + 1];
                normal += GfCross(v0, v1);
            }
            if (i > 0 && j < ydiv - 1) {
                GfVec3f v0 = vertices[j * xdiv + i] - vertices[(j + 1) * xdiv + i - 1];
                GfVec3f v1 = vertices[(j + 1) * xdiv + i] - vertices[(j + 1) * xdiv + i - 1];
                normal += GfCross(v0, v1);
            }
            if (i < xdiv - 1 && j < ydiv - 1) {
                GfVec3f v0 = vertices[j * xdiv + i] - vertices[(j + 1) * xdiv + i + 1];
                GfVec3f v1 = vertices[(j + 1) * xdiv + i] - vertices[(j + 1) * xdiv + i + 1];
                normal += GfCross(v1, v0);
            }
            normals[j * xdiv + i] = normal.GetNormalized();
        }
    }

    for (int j = 0; j < ydiv - 1; ++j) {
        for (int i = 0; i < xdiv - 1; ++i) {
            auto dst = std::span<int>(indices).subspan((j * (xdiv - 1) + i) * 6, 6);
            dst[0] = j * xdiv + i;
            dst[1] = j * xdiv + (i + 1);
            dst[2] = (j + 1) * xdiv + i;
            dst[3] = (j + 1) * xdiv + i;
            dst[4] = j * xdiv + (i + 1);
            dst[5] = (j + 1) * xdiv + (i + 1);
        }
    }
    return true;
}

UsdTerrainGenerator::UsdTerrainGenerator(SdfLayer* layer)
    : _layer(layer)
{
    _params.Parse(layer->GetFileFormatArguments());
}

bool UsdTerrainGenerator::Generate()
{
    auto image = HioImage::OpenForReading(_params.MapFile);
    if (!image) {
        TF_WARN("Failed to open image file: %s", _params.MapFile.c_str());
        return false;
    }

    HioImage::StorageSpec imageData;
    imageData.width = image->GetWidth();
    imageData.height = image->GetHeight();
    imageData.format = image->GetFormat();

    size_t dataSize = imageData.width * imageData.height * image->GetBytesPerPixel();
    std::shared_ptr<void> rawData = std::shared_ptr<void>(malloc(dataSize), free);
    imageData.data = rawData.get();

    if (!image->Read(imageData)) {
        TF_WARN("Failed to get image storage: %s", _params.MapFile.c_str());
        return false;
    }

    if (_params.XDiv < 0) {
        _params.XDiv = image->GetWidth();
    }
    if (_params.YDiv < 0) {
        _params.YDiv = image->GetHeight();
    }

    auto rootPrim = SdfPrimSpec::New(_layer->GetPseudoRoot(), "Root", SdfSpecifierDef);
    auto geomPrim = SdfPrimSpec::New(rootPrim, "Geom", SdfSpecifierDef);
    auto lodSet = SdfVariantSetSpec::New(geomPrim, "lod");

    for (int l = 0; l < _params.MaxLodLevel; ++l) {
        int xdiv = std::max(2, _params.XDiv >> (_params.LodShift * l));
        int ydiv = std::max(2, _params.YDiv >> (_params.LodShift * l));
        if (xdiv < 2 || ydiv < 2) {
            _params.MaxLodLevel = l;
            _params.DefaultLodLevel = std::min(_params.DefaultLodLevel, l - 1);
            break;
        }

        MeshData meshData;
        if (!meshData.Generate(imageData, _params, xdiv, ydiv)) {
            TF_WARN("Failed to generate mesh for LOD %d", l);
            return false;
        }

        auto lodToken = TfToken(std::format("LOD{}", l));
        geomPrim->GetVariantSetNameList().Add(lodToken);
        auto lodVariant = SdfVariantSpec::New(lodSet, lodToken);
        auto lodPrim = SdfPrimSpec::New(lodVariant->GetPrimSpec(), lodToken, SdfSpecifierDef);
        auto meshPrim = SdfPrimSpec::New(lodPrim, "Mesh", SdfSpecifierDef, "Mesh");

        if (auto countsAttr = SdfAttributeSpec::New(meshPrim, UsdGeomTokens->faceVertexCounts, SdfValueTypeNames->IntArray)) {
            countsAttr->SetDefaultValue(VtValue(meshData.counts));
        }
        if (auto indicesAttr = SdfAttributeSpec::New(meshPrim, UsdGeomTokens->faceVertexIndices, SdfValueTypeNames->IntArray)) {
            indicesAttr->SetDefaultValue(VtValue(meshData.indices));
        }
        if (auto pointsAttr = SdfAttributeSpec::New(meshPrim, UsdGeomTokens->points, SdfValueTypeNames->Point3fArray)) {
            pointsAttr->SetDefaultValue(VtValue(meshData.vertices));
        }
        if (auto normalsAttr = SdfAttributeSpec::New(meshPrim, UsdGeomTokens->normals, SdfValueTypeNames->Normal3fArray)) {
            normalsAttr->SetDefaultValue(VtValue(meshData.normals));
        }
        if (auto uvsAttr = SdfAttributeSpec::New(meshPrim, TfToken("primvars:st"), SdfValueTypeNames->TexCoord2fArray)) {
            uvsAttr->SetDefaultValue(VtValue(meshData.uvs));
        }
        if (auto subdivAttr = SdfAttributeSpec::New(meshPrim, UsdGeomTokens->subdivisionScheme, SdfValueTypeNames->Token)) {
            subdivAttr->SetDefaultValue(VtValue(UsdGeomTokens->none));
        }
    }

    geomPrim->SetVariantSelection("lod", std::format("LOD{}", _params.DefaultLodLevel));

    return true;
}

#pragma endregion UsdTerrainGenerator


#pragma region UsdTerrainFileFormat

UsdTerrainFileFormat::UsdTerrainFileFormat()
    : SdfFileFormat(
        UsdTerrainFileFormatTokens->Id,
        UsdTerrainFileFormatTokens->Version,
        UsdTerrainFileFormatTokens->Target,
        UsdTerrainFileFormatTokens->Extension)
{}

UsdTerrainFileFormat::~UsdTerrainFileFormat()
{}

bool UsdTerrainFileFormat::CanRead(const std::string& file) const
{
    return file.ends_with(UsdTerrainFileFormatTokens->Extension.GetString());
}

bool UsdTerrainFileFormat::Read(SdfLayer* layer, const std::string& resolvedPath, bool metadataOnly) const
{
    UsdTerrainGenerator generator(layer);
    return generator.Generate();
}

bool UsdTerrainFileFormat::WriteToString(const SdfLayer& layer, std::string* str, const std::string& comment) const
{
    return false;
}

bool UsdTerrainFileFormat::WriteToStream(const SdfSpecHandle& spec, std::ostream& out, size_t indent) const
{
    return false;
}

void UsdTerrainFileFormat::ComposeFieldsForFileFormatArguments(
    const std::string& assetPath,
    const PcpDynamicFileFormatContext& context,
    FileFormatArguments* args,
    VtValue* contextDependencyData) const
{
    if (!args || !contextDependencyData || !contextDependencyData->IsHolding<VtDictionary>())
        return;

    auto& tokens = UsdTerrainFileFormatTokens;
    auto& dict = contextDependencyData->UncheckedGet<VtDictionary>();

#define LookupAndSetArg(name, type, _) if (auto value = Lookup<type>(dict, tokens->name)) { (*args)[tokens->name] = ToString(*value); }
    USD_TERRAIN_PARAMS_EACH(LookupAndSetArg)
#undef LookupAndSetArg
}

bool UsdTerrainFileFormat::CanFieldChangeAffectFileFormatArguments(
    const TfToken& field,
    const VtValue& oldValue,
    const VtValue& newValue,
    const VtValue& contextDependencyData) const
{
    if (!oldValue.IsHolding<VtDictionary>() || !newValue.IsHolding<VtDictionary>())
        return false;

    auto& oldDict = oldValue.UncheckedGet<VtDictionary>();
    auto& newDict = newValue.UncheckedGet<VtDictionary>();
    auto oldIt = oldDict.find(field);
    auto newIt = newDict.find(field);
    if (oldIt == oldDict.end() && newIt == newDict.end()) {
        return false;
    }

    auto check = [&](const auto& defaultValue) -> bool {
        using T = std::decay_t<decltype(defaultValue)>;
        T oldVal = defaultValue;
        T newVal = defaultValue;
        if (oldIt->second.IsHolding<T>()) {
            oldVal = oldIt->second.UncheckedGet<T>();
        }
        if (newIt->second.IsHolding<T>()) {
            newVal = newIt->second.UncheckedGet<T>();
        }
        return oldVal != newVal;
        };

#define Body(name, type, defaultValue) if (field == UsdTerrainFileFormatTokens->name) { return check(type(defaultValue)); }
    USD_TERRAIN_PARAMS_EACH(Body)
#undef Body

        return false;
}

#pragma endregion UsdTerrainFileFormat
PXR_NAMESPACE_CLOSE_SCOPE
