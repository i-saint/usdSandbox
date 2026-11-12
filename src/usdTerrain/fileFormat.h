#pragma once

PXR_NAMESPACE_OPEN_SCOPE

#define USD_TERRAIN_FILE_FORMAT_TOKENS      \
    ((Id, "usdTerrain"))                    \
    ((Version, "1.0"))                      \
    ((Target, "usd"))                       \
    ((Extension, "usdterrain"))             \
    ((Params, "UsdTerrainParams"))          \
    ((MapFile, "mapFile"))                  \
    ((XDiv, "xDiv"))                        \
    ((YDiv, "yDiv"))                        \
    ((XSize, "xSize"))                      \
    ((YSize, "ySize"))                      \
    ((MaxXRange, "maxXRange"))              \
    ((MaxYRange, "maxYRange"))              \
    ((MaxHeight, "maxHeight"))              \
    ((MaxLodLevel, "maxLodLevel"))          \
    ((DefaultLodLevel, "defaultLodLevel"))  \
    ((LodShift, "lodShift"))                \

#define USD_TERRAIN_PARAMS_EACH(Body)   \
    Body(MapFile, std::string, "")      \
    Body(XDiv, int, -1)                 \
    Body(YDiv, int, -1)                 \
    Body(XSize, float, 100.0f)          \
    Body(YSize, float, 100.0f)          \
    Body(MaxXRange, float, 10.0f)       \
    Body(MaxYRange, float, 10.0f)       \
    Body(MaxHeight, float, 10.0f)       \
    Body(MaxLodLevel, int, 4)           \
    Body(DefaultLodLevel, int, 0)       \
    Body(LodShift, int, 2)              \

TF_DECLARE_PUBLIC_TOKENS(UsdTerrainFileFormatTokens,
    USD_TERRAIN_FILE_FORMAT_TOKENS);


class UsdTerrainFileFormat final : public SdfFileFormat, public PcpDynamicFileFormatInterface
{
public:
    UsdTerrainFileFormat();
    ~UsdTerrainFileFormat() override;

    bool CanRead(const std::string& file) const override;
    bool Read(SdfLayer* layer, const std::string& resolvedPath, bool metadataOnly) const override;

    bool WriteToString(const SdfLayer& layer, std::string* str, const std::string& comment = {}) const override;
    bool WriteToStream(const SdfSpecHandle& spec, std::ostream& out, size_t indent) const override;

    void ComposeFieldsForFileFormatArguments(
        const std::string& assetPath,
        const PcpDynamicFileFormatContext& context,
        FileFormatArguments* args,
        VtValue* contextDependencyData) const override;

    bool CanFieldChangeAffectFileFormatArguments(
        const TfToken& field,
        const VtValue& oldValue,
        const VtValue& newValue,
        const VtValue& contextDependencyData) const override;
};

PXR_NAMESPACE_CLOSE_SCOPE
