#include "pch.h"

#include <algorithm>
#include <cstring>
#include <utility>

class HioLodImage final : public HioImage
{
public:
    using Base = HioImage;

    bool Read(const StorageSpec& storage) override;
    bool ReadCropped(int const cropTop, int const cropBottom, int const cropLeft, int const cropRight, const StorageSpec& storage) override;
    bool Write(const StorageSpec&, const VtDictionary&) override;

    const std::string& GetFilename() const override { return _filename; }
    int GetWidth() const override { return _width; }
    int GetHeight() const override { return _height; }
    HioFormat GetFormat() const override { return _format; }
    int GetBytesPerPixel() const override { return static_cast<int>(HioGetDataSizeOfFormat(_format)); }
    int GetNumMipLevels() const override { return 1; }
    bool IsColorSpaceSRGB() const override { return _isSRGB; }
    bool GetMetadata(const TfToken& name, VtValue* value) const override;
    bool GetSamplerMetadata(HioAddressDimension, HioAddressMode*) const override;

protected:
    bool _OpenForReading(std::string const& filename, int subimage, int mip, HioImage::SourceColorSpace sourceColorSpace, bool suppressErrors) override;
    bool _OpenForWriting(std::string const& filename) override;

private:
    std::string _filename;
    int _width = 0;
    int _height = 0;
    int _lod = 0;
    HioFormat _format = HioFormatInvalid;
    bool _isSRGB = false;
    std::vector<char> _data;
};


TF_REGISTRY_FUNCTION(TfType)
{
    using Image = HioLodImage;
    TfType t = TfType::Define<Image, TfType::Bases<Image::Base>>();
    t.SetFactory<HioImageFactory<Image>>();
}

bool HioLodImage::Read(const StorageSpec& storage)
{
    if (!storage.data || storage.width != _width || storage.height != _height || storage.format != _format) {
        return false;
    }

    std::memcpy(storage.data, _data.data(), _data.size());
    return true;
}

bool HioLodImage::ReadCropped(int const cropTop, int const cropBottom, int const cropLeft, int const cropRight, const StorageSpec& storage)
{
    if (!storage.data || storage.format != _format) {
        return false;
    }

    if (cropTop < 0 || cropBottom < 0 || cropLeft < 0 || cropRight < 0) {
        return false;
    }

    const int croppedWidth = _width - cropLeft - cropRight;
    const int croppedHeight = _height - cropTop - cropBottom;
    if (croppedWidth <= 0 || croppedHeight <= 0) {
        return false;
    }
    if (storage.width != croppedWidth || storage.height != croppedHeight) {
        return false;
    }

    const size_t bytesPerPixel = HioGetDataSizeOfFormat(_format);
    const size_t srcRowBytes = _width * bytesPerPixel;
    const size_t dstRowBytes = croppedWidth * bytesPerPixel;
    const char* srcBase = _data.data();
    auto* dstBase = static_cast<char*>(storage.data);

    for (int y = 0; y < croppedHeight; ++y) {
        const int srcY = cropTop + y;
        const size_t srcOffset = srcY * srcRowBytes + cropLeft * bytesPerPixel;
        const size_t dstOffset = y * dstRowBytes;
        std::memcpy(dstBase + dstOffset, srcBase + srcOffset, dstRowBytes);
    }
    return true;
}

bool HioLodImage::Write(StorageSpec const&, VtDictionary const&)
{
    return false;
}

bool HioLodImage::GetMetadata(const TfToken& name, VtValue* value) const
{
    if (name == "lod") {
        if (value) {
            *value = _lod;
        }
        return true;
    }
    return false;
}

bool HioLodImage::GetSamplerMetadata(HioAddressDimension, HioAddressMode*) const
{
    return false;
}

static bool GenLodImage(const HioImageSharedPtr& src, int lod, std::vector<char>& dstData, int& dstWidth, int& dstHeight);

bool HioLodImage::_OpenForReading(const std::string& filename, int subimage, int mip, SourceColorSpace sourceColorSpace, bool suppressErrors)
{
    // filename は "foobar.png.lod?lod=1" のように、
    // 本来の拡張子の後に .lod がつき、?lod=1 のようにクエリパラメータが続く

    // 本来のファイル名を抽出
    std::string_view baseFilename = filename;
    baseFilename = baseFilename.substr(0, baseFilename.find_last_of('.'));

    HioImageSharedPtr srcImage = HioImage::OpenForReading(std::string(baseFilename));
    if (!srcImage) {
        return false;
    }

    // クエリパラメータ解析
    std::string_view options = filename.substr(baseFilename.find_last_of('?'));
    {
        size_t pos = options.find("lod=");
        if (pos != std::string_view::npos) {
            size_t endPos = options.find('&', pos);
            std::string_view lodStr = options.substr(pos + 4, endPos - (pos + 4));
            _lod = std::stoi(std::string(lodStr));
        }
    }

    // 縮小イメージ生成
    if (GenLodImage(srcImage, _lod, _data, _width, _height)) {
        _filename = filename;
        _format = srcImage->GetFormat();
        _isSRGB = (sourceColorSpace == SourceColorSpace::SRGB);
        return true;
    }
    return false;
}

bool HioLodImage::_OpenForWriting(const std::string& filename)
{
    return false;
}


template <class T>
concept PixelElement = std::is_arithmetic_v<T> || std::is_same_v<T, GfHalf>;

template <int N>
concept LodScale = (N == 1 || N == 2 || N == 4 || N == 8 || N == 16 || N == 32);


template <PixelElement T>
struct LodTraits
{
    // HioTypeInt や HioTypeUnsignedInt だとオーバーフローしうるが、これらが使われることはほぼないので int で様子見
    using AccumType =
        std::conditional_t<std::is_integral_v<T>, int,
        std::conditional_t<std::is_floating_point_v<T>, T,
        std::conditional_t<std::is_same_v<T, GfHalf>, float,
        void>>>;

    static AccumType ToAccum(const T& value)
    {
        return static_cast<AccumType>(value);
    }

    static T FromAccum(AccumType value)
    {
        return static_cast<T>(value);
    }
};

template <int N, PixelElement T>
    requires LodScale<N>
static void DownsampleNxN(const T* src, int srcWidth, int srcHeight, int componentCount, T* dst, int dstWidth, int dstHeight)
{
    if constexpr (N == 1) {
        size_t dataSize = srcWidth * srcHeight * componentCount * sizeof(T);
        std::memcpy(dst, src, dataSize);
        return;
    }
    else {
        using Traits = LodTraits<T>;
        using AccumType = typename Traits::AccumType;
        constexpr AccumType kDivisor = AccumType(N * N);

        for (int y = 0; y < dstHeight; ++y) {
            const int baseY = y * N;

            for (int x = 0; x < dstWidth; ++x) {
                const int baseX = x * N;
                T* out = dst + (y * dstWidth + x) * componentCount;

                // コンポーネントごとに N x N の平均を計算
                for (int c = 0; c < componentCount; ++c) {
                    AccumType sum = AccumType(0);
                    for (int ky = 0; ky < N; ++ky) {
                        const int srcY = std::min(baseY + ky, srcHeight - 1);
                        for (int kx = 0; kx < N; ++kx) {
                            const int srcX = std::min(baseX + kx, srcWidth - 1);
                            const T* p = src + (srcY * srcWidth + srcX) * componentCount;
                            sum += Traits::ToAccum(p[c]);
                        }
                    }
                    out[c] = Traits::FromAccum(sum / kDivisor);
                }
            }
        }
    }
}

template <int N, PixelElement T>
    requires LodScale<N>
static bool DownsampleTyped(const void* src, int srcWidth, int srcHeight, int componentCount, std::vector<char>& dstData, int& dstWidth, int& dstHeight)
{
    dstWidth = std::max(1, srcWidth / N);
    dstHeight = std::max(1, srcHeight / N);
    size_t dstSize = dstWidth * dstHeight * componentCount * sizeof(T);
    dstData.resize(dstSize);

    DownsampleNxN<N>(static_cast<const T*>(src), srcWidth, srcHeight, componentCount, reinterpret_cast<T*>(dstData.data()), dstWidth, dstHeight);
    return true;
}

template <int N>
static bool DownsampleImage(HioFormat format, const void* src, int srcWidth, int srcHeight, std::vector<char>& dstData, int& dstWidth, int& dstHeight)
{
    if (HioIsCompressed(format)) {
        return false;
    }

    const int componentCount = HioGetComponentCount(format);
    if (componentCount <= 0) {
        return false;
    }

    switch (HioGetHioType(format)) {
    case HioTypeUnsignedByte:
    case HioTypeUnsignedByteSRGB:
        return DownsampleTyped<N, uint8_t>(src, srcWidth, srcHeight, componentCount, dstData, dstWidth, dstHeight);
    case HioTypeSignedByte:
        return DownsampleTyped<N, int8_t>(src, srcWidth, srcHeight, componentCount, dstData, dstWidth, dstHeight);
    case HioTypeUnsignedShort:
        return DownsampleTyped<N, uint16_t>(src, srcWidth, srcHeight, componentCount, dstData, dstWidth, dstHeight);
    case HioTypeSignedShort:
        return DownsampleTyped<N, int16_t>(src, srcWidth, srcHeight, componentCount, dstData, dstWidth, dstHeight);
    case HioTypeUnsignedInt:
        return DownsampleTyped<N, uint32_t>(src, srcWidth, srcHeight, componentCount, dstData, dstWidth, dstHeight);
    case HioTypeInt:
        return DownsampleTyped<N, int32_t>(src, srcWidth, srcHeight, componentCount, dstData, dstWidth, dstHeight);
    case HioTypeHalfFloat:
        return DownsampleTyped<N, GfHalf>(src, srcWidth, srcHeight, componentCount, dstData, dstWidth, dstHeight);
    case HioTypeFloat:
        return DownsampleTyped<N, float>(src, srcWidth, srcHeight, componentCount, dstData, dstWidth, dstHeight);
    case HioTypeDouble:
        return DownsampleTyped<N, double>(src, srcWidth, srcHeight, componentCount, dstData, dstWidth, dstHeight);
    default:
        return false;
    }
}

static bool DownsampleImage(HioFormat format, const void* src, int srcWidth, int srcHeight, int lod, std::vector<char>& dstData, int& dstWidth, int& dstHeight)
{
    switch (lod) {
    case 0: return DownsampleImage<1>(format, src, srcWidth, srcHeight, dstData, dstWidth, dstHeight);
    case 1: return DownsampleImage<2>(format, src, srcWidth, srcHeight, dstData, dstWidth, dstHeight);
    case 2: return DownsampleImage<4>(format, src, srcWidth, srcHeight, dstData, dstWidth, dstHeight);
    case 3: return DownsampleImage<8>(format, src, srcWidth, srcHeight, dstData, dstWidth, dstHeight);
    case 4: return DownsampleImage<16>(format, src, srcWidth, srcHeight, dstData, dstWidth, dstHeight);
    case 5: return DownsampleImage<32>(format, src, srcWidth, srcHeight, dstData, dstWidth, dstHeight);
    default: return false;
    }
}

static bool GenLodImage(const HioImageSharedPtr& src, int lod, std::vector<char>& dstData, int& dstWidth, int& dstHeight)
{
    if (!src) {
        return false;
    }

    const HioFormat format = src->GetFormat();
    if (HioIsCompressed(format)) {
        return false;
    }

    int baseWidth = src->GetWidth();
    int baseHeight = src->GetHeight();
    int finalWidth = 0;
    int finalHeight = 0;

    // 最低でも kMinSize のサイズを維持するように mip レベルを調整
    constexpr int kMinSize = 16;
    lod = std::clamp(lod, 0, 5);
    for (; lod > 0; --lod) {
        finalWidth = baseWidth >> lod;
        finalHeight = baseHeight >> lod;
        if (finalWidth >= kMinSize && finalHeight >= kMinSize) {
            break;
        }
    }

    size_t srcSize = src->GetWidth() * src->GetHeight() * HioGetDataSizeOfFormat(format);
    std::vector<char> srcData(srcSize);

    HioImage::StorageSpec srcStorage;
    srcStorage.width = src->GetWidth();
    srcStorage.height = src->GetHeight();
    srcStorage.depth = 1;
    srcStorage.format = format;
    srcStorage.data = srcData.data();
    if (!src->Read(srcStorage)) {
        return false;
    }

    return DownsampleImage(format, srcData.data(), srcStorage.width, srcStorage.height, lod, dstData, dstWidth, dstHeight);
}

