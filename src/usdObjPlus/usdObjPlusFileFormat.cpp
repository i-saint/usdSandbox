#include "usdObjPlusFileFormat.h"

#include "pxr/base/gf/range3f.h"
#include "pxr/base/gf/vec3d.h"
#include "pxr/base/tf/registryManager.h"
#include "pxr/base/vt/array.h"
#include "pxr/usd/sdf/layer.h"
#include "pxr/usd/sdf/path.h"
#include "pxr/usd/usd/primRange.h"
#include "pxr/usd/usd/stage.h"
#include "pxr/usd/usdGeom/mesh.h"
#include "pxr/usd/usdGeom/primvarsAPI.h"
#include "pxr/usd/usdGeom/tokens.h"
#include "pxr/usd/usdGeom/xformCache.h"

#include <charconv>
#include <cctype>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

PXR_NAMESPACE_OPEN_SCOPE

TF_DEFINE_PUBLIC_TOKENS(
    UsdObjPlusFileFormatTokens,
    USD_OBJ_PLUS_FILE_FORMAT_TOKENS);

TF_REGISTRY_FUNCTION(TfType)
{
    SDF_DEFINE_FILE_FORMAT(UsdObjPlusFileFormat, SdfFileFormat);
}

namespace {
struct ObjCorner
{
    int pointIndex = -1;
    int uvIndex = -1;
};

struct ObjFace
{
    std::vector<ObjCorner> corners;
};

struct ObjGroup
{
    std::string name;
    std::vector<ObjFace> faces;
};

bool ParseObjIndex(std::string_view text, size_t count, int* index)
{
    int value = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size() || value == 0)
        return false;

    const long long resolved = value > 0
        ? static_cast<long long>(value) - 1
        : static_cast<long long>(count) + value;
    if (resolved < 0 || resolved >= static_cast<long long>(count))
        return false;

    *index = static_cast<int>(resolved);
    return true;
}

bool ParseObjCorner(const std::string& token, size_t pointCount, size_t uvCount, ObjCorner* corner)
{
    const auto firstSlash = token.find('/');
    const std::string_view pointText(token.data(), firstSlash == std::string::npos ? token.size() : firstSlash);
    if (!ParseObjIndex(pointText, pointCount, &corner->pointIndex))
        return false;

    if (firstSlash != std::string::npos) {
        const auto uvStart = firstSlash + 1;
        const auto secondSlash = token.find('/', uvStart);
        if (secondSlash != uvStart) {
            const std::string_view uvText(token.data() + uvStart,
                (secondSlash == std::string::npos ? token.size() : secondSlash) - uvStart);
            ParseObjIndex(uvText, uvCount, &corner->uvIndex);
        }
    }
    return true;
}

std::string MakePrimName(const std::string& name)
{
    std::string result;
    result.reserve(name.size());
    for (unsigned char ch : name) {
        result.push_back(std::isalnum(ch) || ch == '_' ? static_cast<char>(ch) : '_');
    }
    if (result.empty())
        result = "Mesh";
    if (!std::isalpha(static_cast<unsigned char>(result.front())) && result.front() != '_')
        result.insert(result.begin(), '_');
    return result;
}

bool ParseObj(std::istream& input, std::vector<GfVec3f>* points, std::vector<GfVec2f>* uvs,
    std::vector<ObjGroup>* groups)
{
    std::unordered_map<std::string, size_t> groupIndices;
    auto getGroup = [&](const std::string& name) -> ObjGroup& {
        const auto found = groupIndices.find(name);
        if (found != groupIndices.end())
            return (*groups)[found->second];
        const size_t index = groups->size();
        groupIndices.emplace(name, index);
        groups->push_back({ name, {} });
        return groups->back();
    };

    ObjGroup* currentGroup = &getGroup("default");
    std::string line;
    while (std::getline(input, line)) {
        std::istringstream lineStream(line);
        std::string type;
        lineStream >> type;
        if (type.empty() || type[0] == '#')
            continue;

        if (type == "v") {
            GfVec3f point;
            if (!(lineStream >> point[0] >> point[1] >> point[2]))
                return false;
            points->push_back(point);
        }
        else if (type == "vt") {
            GfVec2f uv;
            if (!(lineStream >> uv[0] >> uv[1]))
                return false;
            uvs->push_back(uv);
        }
        else if (type == "g" || type == "o") {
            std::string name;
            lineStream >> name;
            currentGroup = &getGroup(name.empty() ? "default" : name);
        }
        else if (type == "f") {
            ObjFace face;
            std::string token;
            while (lineStream >> token) {
                ObjCorner corner;
                if (!ParseObjCorner(token, points->size(), uvs->size(), &corner))
                    return false;
                face.corners.push_back(corner);
            }
            if (face.corners.size() < 3)
                return false;
            currentGroup->faces.push_back(std::move(face));
        }
    }
    return !input.bad();
}

SdfLayerRefPtr TranslateObjToUsd(
    const std::vector<GfVec3f>& points,
    const std::vector<GfVec2f>& uvs,
    const std::vector<ObjGroup>& groups)
{
    SdfLayerRefPtr layer = SdfLayer::CreateAnonymous(".usda");
    UsdStageRefPtr stage = UsdStage::Open(layer);
    if (!stage)
        return {};

    GfRange3f extent;
    for (const auto& point : points)
        extent.UnionWith(point);
    VtVec3fArray extentArray;
    if (!points.empty()) {
        extentArray.push_back(extent.GetMin());
        extentArray.push_back(extent.GetMax());
    }

    std::unordered_set<std::string> usedNames;
    for (const auto& group : groups) {
        if (group.faces.empty())
            continue;

        std::string name = MakePrimName(group.name);
        const std::string baseName = name;
        for (size_t suffix = 1; !usedNames.insert(name).second; ++suffix)
            name = baseName + "_" + std::to_string(suffix);

        const SdfPath path = SdfPath::AbsoluteRootPath().AppendChild(TfToken(name));
        UsdGeomMesh mesh = UsdGeomMesh::Define(stage, path);
        VtVec3fArray usdPoints(points.begin(), points.end());
        VtIntArray faceVertexCounts;
        VtIntArray faceVertexIndices;
        VtIntArray faceUvIndices;
        bool allCornersHaveUvs = !uvs.empty();
        for (const auto& face : group.faces) {
            faceVertexCounts.push_back(static_cast<int>(face.corners.size()));
            for (const auto& corner : face.corners) {
                faceVertexIndices.push_back(corner.pointIndex);
                if (corner.uvIndex < 0)
                    allCornersHaveUvs = false;
                else
                    faceUvIndices.push_back(corner.uvIndex);
            }
        }

        mesh.GetPointsAttr().Set(usdPoints);
        mesh.GetFaceVertexCountsAttr().Set(faceVertexCounts);
        mesh.GetFaceVertexIndicesAttr().Set(faceVertexIndices);
        mesh.GetSubdivisionSchemeAttr().Set(UsdGeomTokens->none);
        if (!extentArray.empty())
            mesh.GetExtentAttr().Set(extentArray);

        if (allCornersHaveUvs && faceUvIndices.size() == faceVertexIndices.size()) {
            VtVec2fArray usdUvs(uvs.begin(), uvs.end());
            UsdGeomPrimvar uvPrimvar = UsdGeomPrimvarsAPI(mesh).CreatePrimvar(
                TfToken("uv"), SdfValueTypeNames->TexCoord2fArray, UsdGeomTokens->faceVarying);
            uvPrimvar.Set(usdUvs);
            uvPrimvar.SetIndices(faceUvIndices);
        }
    }
    return layer;
}
}

UsdObjPlusFileFormat::UsdObjPlusFileFormat()
    : SdfFileFormat(
        UsdObjPlusFileFormatTokens->Id,
        UsdObjPlusFileFormatTokens->Version,
        UsdObjPlusFileFormatTokens->Target,
        UsdObjPlusFileFormatTokens->Extension)
{}

UsdObjPlusFileFormat::~UsdObjPlusFileFormat() = default;

bool UsdObjPlusFileFormat::CanRead(const std::string& file) const
{
    return file.ends_with(".obj") || file.ends_with(".OBJ");
}

bool UsdObjPlusFileFormat::_ReadFromStream(SdfLayer* layer, std::istream& input, bool) const
{
    std::vector<GfVec3f> points;
    std::vector<GfVec2f> uvs;
    std::vector<ObjGroup> groups;
    if (!ParseObj(input, &points, &uvs, &groups))
        return false;

    SdfLayerRefPtr translated = TranslateObjToUsd(points, uvs, groups);
    if (!translated)
        return false;
    layer->TransferContent(translated);
    return true;
}

bool UsdObjPlusFileFormat::Read(SdfLayer* layer, const std::string& resolvedPath, bool metadataOnly) const
{
    std::ifstream input(resolvedPath);
    if (!input.is_open()) {
        TF_RUNTIME_ERROR("Failed to open OBJ file '%s'", resolvedPath.c_str());
        return false;
    }
    if (!_ReadFromStream(layer, input, metadataOnly)) {
        TF_RUNTIME_ERROR("Failed to parse OBJ file '%s'", resolvedPath.c_str());
        return false;
    }
    return true;
}

bool UsdObjPlusFileFormat::ReadFromString(SdfLayer* layer, const std::string& str) const
{
    std::istringstream input(str);
    return _ReadFromStream(layer, input, false);
}

bool UsdObjPlusFileFormat::WriteToFile(
    const SdfLayer& layer,
    const std::string& filePath,
    const std::string&,
    const FileFormatArguments&) const
{
    SdfLayerHandle rootLayer = SdfLayer::Find(layer.GetIdentifier());
    if (!rootLayer)
        return false;

    UsdStageRefPtr stage = UsdStage::Open(rootLayer);
    if (!stage)
        return false;

    std::ofstream output(filePath, std::ios::out | std::ios::trunc);
    if (!output.is_open())
        return false;

    output << std::setprecision(std::numeric_limits<double>::max_digits10);
    UsdGeomXformCache xformCache(UsdTimeCode::Default());
    size_t vertexOffset = 0;
    for (const UsdPrim& prim : stage->Traverse()) {
        UsdGeomMesh mesh(prim);
        if (!mesh)
            continue;

        VtVec3fArray points;
        VtIntArray faceVertexCounts;
        VtIntArray faceVertexIndices;
        if (!mesh.GetPointsAttr().Get(&points)
            || !mesh.GetFaceVertexCountsAttr().Get(&faceVertexCounts)
            || !mesh.GetFaceVertexIndicesAttr().Get(&faceVertexIndices))
            return false;

        size_t expectedIndexCount = 0;
        for (int count : faceVertexCounts) {
            if (count < 3)
                return false;
            expectedIndexCount += static_cast<size_t>(count);
        }
        if (expectedIndexCount != faceVertexIndices.size())
            return false;
        for (int index : faceVertexIndices) {
            if (index < 0 || static_cast<size_t>(index) >= points.size())
                return false;
        }

        const GfMatrix4d transform = xformCache.GetLocalToWorldTransform(prim);
        output << "g " << prim.GetName().GetString() << '\n';
        for (const GfVec3f& point : points) {
            const GfVec3d worldPoint = transform.Transform(
                GfVec3d(point[0], point[1], point[2]));
            output << "v " << worldPoint[0] << ' ' << worldPoint[1] << ' ' << worldPoint[2] << '\n';
        }

        size_t indexOffset = 0;
        for (int count : faceVertexCounts) {
            output << 'f';
            for (int corner = 0; corner < count; ++corner) {
                const size_t index = static_cast<size_t>(faceVertexIndices[indexOffset++]);
                output << ' ' << vertexOffset + index + 1;
            }
            output << '\n';
        }
        vertexOffset += points.size();
    }
    return output.good();
}

bool UsdObjPlusFileFormat::WriteToString(const SdfLayer&, std::string*, const std::string&) const
{
    return false;
}

bool UsdObjPlusFileFormat::WriteToStream(const SdfSpecHandle&, std::ostream&, size_t) const
{
    return false;
}

PXR_NAMESPACE_CLOSE_SCOPE
