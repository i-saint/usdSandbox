#pragma once

#include "pxr/base/tf/staticTokens.h"
#include "pxr/usd/sdf/fileFormat.h"

#include <iosfwd>
#include <string>

PXR_NAMESPACE_OPEN_SCOPE

#define USD_OBJ_PLUS_FILE_FORMAT_TOKENS \
    ((Id, "usdObjPlus"))              \
    ((Version, "1.0"))                \
    ((Target, "usd"))                 \
    ((Extension, "obj"))

TF_DECLARE_PUBLIC_TOKENS(UsdObjPlusFileFormatTokens, USD_OBJ_PLUS_FILE_FORMAT_TOKENS);

class UsdObjPlusFileFormat final : public SdfFileFormat
{
public:
    bool CanRead(const std::string& file) const override;
    bool Read(SdfLayer* layer, const std::string& resolvedPath, bool metadataOnly) const override;
    bool ReadFromString(SdfLayer* layer, const std::string& str) const override;

    bool WriteToFile(
        const SdfLayer& layer,
        const std::string& filePath,
        const std::string& comment = std::string(),
        const FileFormatArguments& args = FileFormatArguments()) const override;

    bool WriteToString(
        const SdfLayer& layer,
        std::string* str,
        const std::string& comment = std::string()) const override;
    bool WriteToStream(const SdfSpecHandle& spec, std::ostream& out, size_t indent) const override;

protected:
    SDF_FILE_FORMAT_FACTORY_ACCESS;

    UsdObjPlusFileFormat();
    ~UsdObjPlusFileFormat() override;

private:
    bool _ReadFromStream(SdfLayer* layer, std::istream& input, bool metadataOnly) const;
};

PXR_NAMESPACE_CLOSE_SCOPE
