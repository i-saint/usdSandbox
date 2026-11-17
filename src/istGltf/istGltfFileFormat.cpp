#include "../istTerrain/pch.h"
#include "../istTerrain/meshUtils.h"

#include "pxr/base/tf/diagnostic.h"
#include "pxr/base/tf/registryManager.h"
#include "pxr/usd/sdf/layer.h"
#include "pxr/usd/sdf/spec.h"
#include "pxr/usd/usd/primRange.h"
#include "pxr/usd/usd/stage.h"
#include "pxr/usd/usdGeom/mesh.h"
#include "pxr/usd/usdGeom/metrics.h"
#include "pxr/usd/usdGeom/primvarsAPI.h"
#include "pxr/usd/usdGeom/xformCache.h"

#define CGLTF_IMPLEMENTATION
#define CGLTF_WRITE_IMPLEMENTATION
#include <cgltf_write.h>

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <sstream>
#include <unordered_set>

PXR_NAMESPACE_OPEN_SCOPE

static bool ImportGltf(SdfLayer* layer, const std::string& source, bool fromString);

class UsdGltfFileFormat : public SdfFileFormat
{
public:
    bool CanRead(const std::string& path) const override;
    bool Read(SdfLayer* layer, const std::string& path, bool metadataOnly) const override;
    bool ReadFromString(SdfLayer* layer, const std::string& str) const override;
    FileFormatArguments GetDefaultFileFormatArguments() const override { return {{"detriangulate", "false"}}; }
    bool WriteToFile(const SdfLayer& layer, const std::string& path,
        const std::string&, const FileFormatArguments&) const override;
    bool WriteToString(const SdfLayer& layer, std::string* str, const std::string&) const override;
    bool WriteToStream(const SdfSpecHandle& spec, std::ostream& out, size_t indent) const override;

protected:
    SDF_FILE_FORMAT_FACTORY_ACCESS;
    UsdGltfFileFormat() : UsdGltfFileFormat("istGltf", "gltf") {}
    UsdGltfFileFormat(const char* id, const char* extension)
        : SdfFileFormat(TfToken(id), TfToken("1.0"), TfToken("usd"), extension) {}
    ~UsdGltfFileFormat() override = default;
};

class UsdGlbFileFormat final : public UsdGltfFileFormat
{
protected:
    SDF_FILE_FORMAT_FACTORY_ACCESS;
    UsdGlbFileFormat() : UsdGltfFileFormat("istGlb", "glb") {}
    ~UsdGlbFileFormat() override = default;
};

TF_REGISTRY_FUNCTION(TfType)
{
    SDF_DEFINE_FILE_FORMAT(UsdGltfFileFormat, SdfFileFormat);
    SDF_DEFINE_FILE_FORMAT(UsdGlbFileFormat, UsdGltfFileFormat);
}

bool UsdGltfFileFormat::CanRead(const std::string& path) const
{
    std::string extension = std::filesystem::u8path(path).extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return extension == ".gltf" || extension == ".glb";
}

bool UsdGltfFileFormat::Read(SdfLayer* layer, const std::string& path, bool) const
{
    return ImportGltf(layer, path, false);
}

bool UsdGltfFileFormat::ReadFromString(SdfLayer* layer, const std::string& str) const
{
    return ImportGltf(layer, str, true);
}

namespace {
cgltf_result ReadGltfFile(const cgltf_memory_options* memory, const cgltf_file_options*,
    const char* path, cgltf_size* size, void** data)
{
    std::ifstream input(std::filesystem::u8path(path), std::ios::binary | std::ios::ate);
    if (!input) return cgltf_result_file_not_found;
    const auto length = input.tellg();
    if (length < 0 || static_cast<uint64_t>(length) > std::numeric_limits<size_t>::max()) return cgltf_result_io_error;
    const size_t requested = *size ? *size : static_cast<size_t>(length);
    if (requested > static_cast<uint64_t>(length)) return cgltf_result_data_too_short;
    void* buffer = memory->alloc_func ? memory->alloc_func(memory->user_data, requested) : std::malloc(requested);
    if (!buffer) return cgltf_result_out_of_memory;
    input.seekg(0);
    if (!input.read(static_cast<char*>(buffer), requested)) {
        if (memory->free_func) memory->free_func(memory->user_data, buffer); else std::free(buffer);
        return cgltf_result_io_error;
    }
    *size = requested;
    *data = buffer;
    return cgltf_result_success;
}

void ReleaseGltfFile(const cgltf_memory_options* memory, const cgltf_file_options*, void* data)
{
    if (memory->free_func) memory->free_func(memory->user_data, data); else std::free(data);
}

bool GetDetriangulate(const SdfLayer& layer, bool& result)
{
    result = false;
    const auto& args = layer.GetFileFormatArguments();
    const auto it = args.find("detriangulate");
    if (it == args.end() || it->second == "false" || it->second == "0") return true;
    if (it->second == "true" || it->second == "1") { result = true; return true; }
    TF_RUNTIME_ERROR("Invalid detriangulate argument '%s': expected true, false, 1 or 0", it->second.c_str());
    return false;
}

uint32_t ReadUnsigned(const uint8_t* p, cgltf_component_type type)
{
    if (type == cgltf_component_type_r_8u) return *p;
    if (type == cgltf_component_type_r_16u) { uint16_t v; std::memcpy(&v, p, sizeof(v)); return v; }
    uint32_t v; std::memcpy(&v, p, sizeof(v)); return v;
}

float ReadFloatComponent(const uint8_t* p, cgltf_component_type type, bool normalized)
{
    if (type == cgltf_component_type_r_32f) {
        float value;
        std::memcpy(&value, p, sizeof(value));
        return value;
    }
    const uint32_t value = ReadUnsigned(p, type);
    if (!normalized) return static_cast<float>(value);
    if (type == cgltf_component_type_r_8u) return static_cast<float>(value) / 255.0f;
    return static_cast<float>(value) / 65535.0f;
}

bool ReadFloatAttribute(const cgltf_accessor& accessor, cgltf_type type, size_t expectedCount,
    bool allowNormalizedUnsigned, std::vector<float>& values)
{
    const size_t components = cgltf_num_components(type);
    const bool floatComponents = accessor.component_type == cgltf_component_type_r_32f;
    const bool normalizedUnsigned = allowNormalizedUnsigned && accessor.normalized &&
        (accessor.component_type == cgltf_component_type_r_8u || accessor.component_type == cgltf_component_type_r_16u);
    if (accessor.type != type || accessor.count != expectedCount || accessor.count == 0 ||
        accessor.count > static_cast<size_t>(std::numeric_limits<int>::max()) ||
        (!floatComponents && !normalizedUnsigned) ||
        (accessor.buffer_view && accessor.buffer_view->has_meshopt_compression)) return false;

    values.resize(accessor.count * components);
    cgltf_accessor base = accessor;
    base.is_sparse = false;
    if (accessor.buffer_view) {
        if (cgltf_accessor_unpack_floats(&base, values.data(), values.size()) != values.size()) return false;
    }
    else if (!accessor.is_sparse) return false;

    if (accessor.is_sparse) {
        const auto& sparse = accessor.sparse;
        if (!sparse.indices_buffer_view || !sparse.values_buffer_view ||
            sparse.indices_buffer_view->has_meshopt_compression || sparse.values_buffer_view->has_meshopt_compression) return false;
        const uint8_t* indices = cgltf_buffer_view_data(sparse.indices_buffer_view);
        const uint8_t* data = cgltf_buffer_view_data(sparse.values_buffer_view);
        if (!indices || !data) return false;
        indices += sparse.indices_byte_offset;
        data += sparse.values_byte_offset;
        const size_t indexStride = cgltf_component_size(sparse.indices_component_type);
        const size_t componentSize = cgltf_component_size(accessor.component_type);
        for (size_t i = 0; i < sparse.count; ++i) {
            const uint32_t index = ReadUnsigned(indices + i * indexStride, sparse.indices_component_type);
            if (index >= accessor.count) return false;
            for (size_t component = 0; component < components; ++component) {
                values[index * components + component] = ReadFloatComponent(
                    data + (i * components + component) * componentSize, accessor.component_type, accessor.normalized);
            }
        }
    }
    return true;
}

bool ReadPositions(const cgltf_accessor& accessor, std::vector<float>& values)
{
    return accessor.component_type == cgltf_component_type_r_32f &&
        ReadFloatAttribute(accessor, cgltf_type_vec3, accessor.count, false, values);
}

bool ReadIndices(const cgltf_accessor& accessor, std::vector<uint32_t>& values)
{
    if (accessor.type != cgltf_type_scalar || (accessor.component_type != cgltf_component_type_r_8u &&
        accessor.component_type != cgltf_component_type_r_16u && accessor.component_type != cgltf_component_type_r_32u) ||
        (accessor.buffer_view && accessor.buffer_view->has_meshopt_compression)) return false;
    values.assign(accessor.count, 0);
    if (accessor.buffer_view) {
        cgltf_accessor base = accessor;
        base.is_sparse = false;
        if (cgltf_accessor_unpack_indices(&base, values.data(), sizeof(uint32_t), values.size()) != values.size()) return false;
    }
    else if (!accessor.is_sparse) return false;
    if (accessor.is_sparse) {
        const auto& sparse = accessor.sparse;
        if (!sparse.indices_buffer_view || !sparse.values_buffer_view || sparse.indices_buffer_view->has_meshopt_compression || sparse.values_buffer_view->has_meshopt_compression) return false;
        const uint8_t* indices = cgltf_buffer_view_data(sparse.indices_buffer_view);
        const uint8_t* data = cgltf_buffer_view_data(sparse.values_buffer_view);
        if (!indices || !data) return false;
        indices += sparse.indices_byte_offset;
        data += sparse.values_byte_offset;
        const size_t indexSize = cgltf_component_size(sparse.indices_component_type);
        const size_t valueSize = cgltf_component_size(accessor.component_type);
        for (size_t i = 0; i < sparse.count; ++i) {
            const uint32_t index = ReadUnsigned(indices + i * indexSize, sparse.indices_component_type);
            if (index >= accessor.count) return false;
            values[index] = ReadUnsigned(data + i * valueSize, accessor.component_type);
        }
    }
    return true;
}

bool ImportPrimitive(const cgltf_primitive& primitive, const cgltf_node& node, bool detriangulate,
    const UsdStageRefPtr& stage, size_t number)
{
    if (primitive.type != cgltf_primitive_type_triangles && primitive.type != cgltf_primitive_type_triangle_strip &&
        primitive.type != cgltf_primitive_type_triangle_fan) return true;
    if (primitive.has_draco_mesh_compression) {
        TF_RUNTIME_ERROR("Draco-compressed glTF meshes are not supported");
        return false;
    }
    const cgltf_accessor* position = cgltf_find_accessor(&primitive, cgltf_attribute_type_position, 0);
    std::vector<float> values;
    if (!position || !ReadPositions(*position, values)) {
        TF_RUNTIME_ERROR("Invalid or unsupported glTF POSITION accessor");
        return false;
    }
    std::vector<uint32_t> source;
    if (primitive.indices) {
        if (!ReadIndices(*primitive.indices, source)) {
            TF_RUNTIME_ERROR("Invalid or unsupported glTF index accessor");
            return false;
        }
    }
    else {
        source.resize(position->count);
        for (size_t i = 0; i < source.size(); ++i) source[i] = static_cast<uint32_t>(i);
    }
    for (uint32_t index : source) {
        if (index >= position->count) {
            TF_RUNTIME_ERROR("glTF mesh index is out of bounds");
            return false;
        }
    }
    if ((primitive.type == cgltf_primitive_type_triangles && source.size() % 3 != 0) || source.size() < 3 || source.size() > static_cast<size_t>(std::numeric_limits<int>::max() / 3)) {
        TF_RUNTIME_ERROR("Invalid glTF triangle count");
        return false;
    }
    float matrix[16];
    cgltf_node_transform_world(&node, matrix);
    const float determinant = matrix[0] * (matrix[5] * matrix[10] - matrix[9] * matrix[6])
        - matrix[4] * (matrix[1] * matrix[10] - matrix[9] * matrix[2])
        + matrix[8] * (matrix[1] * matrix[6] - matrix[5] * matrix[2]);
    if (!std::isfinite(determinant)) {
        TF_RUNTIME_ERROR("Non-finite glTF node transform");
        return false;
    }
    const cgltf_accessor* normal = cgltf_find_accessor(&primitive, cgltf_attribute_type_normal, 0);
    const cgltf_accessor* tangent = cgltf_find_accessor(&primitive, cgltf_attribute_type_tangent, 0);
    const cgltf_accessor* texcoord0 = cgltf_find_accessor(&primitive, cgltf_attribute_type_texcoord, 0);
    const cgltf_accessor* texcoord1 = cgltf_find_accessor(&primitive, cgltf_attribute_type_texcoord, 1);
    std::vector<float> normalValues, tangentValues, uv0Values, uv1Values;
    if ((normal && !ReadFloatAttribute(*normal, cgltf_type_vec3, position->count, false, normalValues)) ||
        (tangent && !ReadFloatAttribute(*tangent, cgltf_type_vec4, position->count, false, tangentValues)) ||
        (texcoord0 && !ReadFloatAttribute(*texcoord0, cgltf_type_vec2, position->count, true, uv0Values)) ||
        (texcoord1 && !ReadFloatAttribute(*texcoord1, cgltf_type_vec2, position->count, true, uv1Values))) {
        TF_RUNTIME_ERROR("Invalid or unsupported glTF vertex attribute accessor");
        return false;
    }
    if (determinant == 0.0f && normal) {
        TF_RUNTIME_ERROR("Cannot transform glTF normals through a singular node transform");
        return false;
    }
    for (size_t i = 0; i < normalValues.size(); i += 3) {
        const float x = normalValues[i], y = normalValues[i + 1], z = normalValues[i + 2];
        normalValues[i] = ((matrix[5] * matrix[10] - matrix[9] * matrix[6]) * x + (matrix[9] * matrix[2] - matrix[1] * matrix[10]) * y + (matrix[1] * matrix[6] - matrix[5] * matrix[2]) * z) / determinant;
        normalValues[i + 1] = ((matrix[8] * matrix[6] - matrix[4] * matrix[10]) * x + (matrix[0] * matrix[10] - matrix[8] * matrix[2]) * y + (matrix[4] * matrix[2] - matrix[0] * matrix[6]) * z) / determinant;
        normalValues[i + 2] = ((matrix[4] * matrix[9] - matrix[8] * matrix[5]) * x + (matrix[8] * matrix[1] - matrix[0] * matrix[9]) * y + (matrix[0] * matrix[5] - matrix[4] * matrix[1]) * z) / determinant;
        const float length = std::sqrt(normalValues[i] * normalValues[i] + normalValues[i + 1] * normalValues[i + 1] + normalValues[i + 2] * normalValues[i + 2]);
        if (!std::isfinite(length) || length <= 0.0f) {
            TF_RUNTIME_ERROR("Invalid transformed glTF normal");
            return false;
        }
        for (size_t component = 0; component < 3; ++component) normalValues[i + component] /= length;
    }
    for (size_t i = 0; i < tangentValues.size(); i += 4) {
        const float x = tangentValues[i], y = tangentValues[i + 1], z = tangentValues[i + 2];
        tangentValues[i] = matrix[0] * x + matrix[4] * y + matrix[8] * z;
        tangentValues[i + 1] = matrix[1] * x + matrix[5] * y + matrix[9] * z;
        tangentValues[i + 2] = matrix[2] * x + matrix[6] * y + matrix[10] * z;
        const float length = std::sqrt(tangentValues[i] * tangentValues[i] + tangentValues[i + 1] * tangentValues[i + 1] + tangentValues[i + 2] * tangentValues[i + 2]);
        if (!std::isfinite(length) || length <= 0.0f) {
            TF_RUNTIME_ERROR("Invalid transformed glTF tangent");
            return false;
        }
        for (size_t component = 0; component < 3; ++component) tangentValues[i + component] /= length;
        if (determinant < 0.0f) tangentValues[i + 3] = -tangentValues[i + 3];
    }
    auto finiteValues = [](const std::vector<float>& values) {
        return std::all_of(values.begin(), values.end(), [](float value) { return std::isfinite(value); });
    };
    if (!finiteValues(normalValues) || !finiteValues(tangentValues) || !finiteValues(uv0Values) || !finiteValues(uv1Values)) {
        TF_RUNTIME_ERROR("Non-finite glTF vertex attribute");
        return false;
    }
    VtVec3fArray points(position->count);
    for (size_t i = 0; i < points.size(); ++i) {
        const float x = values[i * 3], y = values[i * 3 + 1], z = values[i * 3 + 2];
        points[i] = GfVec3f(matrix[0] * x + matrix[4] * y + matrix[8] * z + matrix[12],
            matrix[1] * x + matrix[5] * y + matrix[9] * z + matrix[13],
            matrix[2] * x + matrix[6] * y + matrix[10] * z + matrix[14]);
        if (!std::isfinite(points[i][0]) || !std::isfinite(points[i][1]) || !std::isfinite(points[i][2])) {
            TF_RUNTIME_ERROR("Non-finite glTF mesh position");
            return false;
        }
    }
    std::vector<int> triangles;
    auto append = [&](uint32_t a, uint32_t b, uint32_t c) {
        triangles.push_back(static_cast<int>(a));
        triangles.push_back(static_cast<int>(determinant < 0 ? c : b));
        triangles.push_back(static_cast<int>(determinant < 0 ? b : c));
    };
    if (primitive.type == cgltf_primitive_type_triangles) {
        for (size_t i = 0; i < source.size(); i += 3) append(source[i], source[i + 1], source[i + 2]);
    }
    else {
        for (size_t i = 2; i < source.size(); ++i) {
            if (primitive.type == cgltf_primitive_type_triangle_fan) append(source[0], source[i - 1], source[i]);
            else if (i % 2 == 0) append(source[i - 2], source[i - 1], source[i]);
            else append(source[i - 1], source[i - 2], source[i]);
        }
    }
    VtIntArray counts, indices;
    if (detriangulate) {
        Detriangulator converter;
        if (!converter.Detriangulate(triangles)) {
            TF_RUNTIME_ERROR("Failed to detriangulate glTF mesh");
            return false;
        }
        counts = converter.counts;
        indices = converter.indices;
    }
    else {
        counts.assign(triangles.size() / 3, 3);
        indices.assign(triangles.begin(), triangles.end());
    }
    const GfRange3f bounds = FastBounds(std::span<const GfVec3f>(points.data(), points.size()));
    const VtVec3fArray extent{bounds.GetMin(), bounds.GetMax()};
    const SdfPath path = SdfPath::AbsoluteRootPath().AppendChild(TfToken("Mesh_" + std::to_string(number)));
    UsdGeomMesh mesh = UsdGeomMesh::Define(stage, path);
    if (!mesh || !mesh.CreatePointsAttr().Set(points) || !mesh.CreateFaceVertexCountsAttr().Set(counts) ||
        !mesh.CreateExtentAttr().Set(extent) || !mesh.CreateFaceVertexIndicesAttr().Set(indices) || !mesh.CreateSubdivisionSchemeAttr().Set(UsdGeomTokens->none)) {
        TF_RUNTIME_ERROR("Failed to create USD mesh '%s'", path.GetText());
        return false;
    }
    if (normal) {
        VtVec3fArray values(position->count);
        for (size_t i = 0; i < values.size(); ++i) values[i] = GfVec3f(normalValues[i * 3], normalValues[i * 3 + 1], normalValues[i * 3 + 2]);
        if (!mesh.CreateNormalsAttr().Set(values)) {
            TF_RUNTIME_ERROR("Failed to create USD mesh normals '%s'", path.GetText());
            return false;
        }
        mesh.SetNormalsInterpolation(UsdGeomTokens->vertex);
    }
    UsdGeomPrimvarsAPI primvars(mesh);
    auto setPrimvar = [&](const char* name, const SdfValueTypeName& type, const std::vector<float>& source, size_t components) {
        if (source.empty()) return true;
        UsdGeomPrimvar primvar = primvars.CreatePrimvar(TfToken(name), type, UsdGeomTokens->vertex);
        if (!primvar) return false;
        if (components == 4) {
            VtVec4fArray values(position->count);
            for (size_t i = 0; i < values.size(); ++i) values[i] = GfVec4f(source[i * 4], source[i * 4 + 1], source[i * 4 + 2], source[i * 4 + 3]);
            return primvar.Set(values);
        }
        if (components == 3) {
            VtVec3fArray values(position->count);
            for (size_t i = 0; i < values.size(); ++i) values[i] = GfVec3f(source[i * 3], source[i * 3 + 1], source[i * 3 + 2]);
            return primvar.Set(values);
        }
        VtVec2fArray values(position->count);
        for (size_t i = 0; i < values.size(); ++i) values[i] = GfVec2f(source[i * 2], source[i * 2 + 1]);
        return primvar.Set(values);
    };
    if (!setPrimvar("tangents", SdfValueTypeNames->Float4Array, tangentValues, 4) ||
        !setPrimvar("st", SdfValueTypeNames->TexCoord2fArray, uv0Values, 2) ||
        !setPrimvar("st1", SdfValueTypeNames->TexCoord2fArray, uv1Values, 2)) {
        TF_RUNTIME_ERROR("Failed to create USD mesh primvars '%s'", path.GetText());
        return false;
    }
    return true;
}

bool TranslateGltf(cgltf_data& data, bool detriangulate, SdfLayer* layer)
{
    SdfLayerRefPtr temporary = SdfLayer::CreateAnonymous(".usda");
    UsdStageRefPtr stage = UsdStage::Open(temporary);
    if (!stage || !UsdGeomSetStageUpAxis(stage, UsdGeomTokens->y) || !UsdGeomSetStageMetersPerUnit(stage, 1.0)) {
        TF_RUNTIME_ERROR("Failed to create USD stage for glTF import");
        return false;
    }
    std::vector<const cgltf_node*> pending;
    const cgltf_scene* scene = data.scene ? data.scene : (data.scenes_count ? &data.scenes[0] : nullptr);
    if (scene) {
        for (size_t i = scene->nodes_count; i > 0; --i) pending.push_back(scene->nodes[i - 1]);
    }
    else {
        for (size_t i = data.nodes_count; i > 0; --i) if (!data.nodes[i - 1].parent) pending.push_back(&data.nodes[i - 1]);
    }
    std::unordered_set<const cgltf_node*> visited;
    size_t number = 0;
    while (!pending.empty()) {
        const cgltf_node* node = pending.back();
        pending.pop_back();
        if (!node || !visited.insert(node).second) {
            TF_RUNTIME_ERROR("glTF scene contains an invalid or repeated node");
            return false;
        }
        if (node->has_mesh_gpu_instancing) {
            TF_RUNTIME_ERROR("EXT_mesh_gpu_instancing is not supported");
            return false;
        }
        if (node->mesh) {
            for (size_t i = 0; i < node->mesh->primitives_count; ++i) {
                if (!ImportPrimitive(node->mesh->primitives[i], *node, detriangulate, stage, number++)) return false;
            }
        }
        for (size_t i = node->children_count; i > 0; --i) pending.push_back(node->children[i - 1]);
    }
    layer->TransferContent(temporary);
    return true;
}

struct ExportMesh
{
    std::string name;
    std::vector<float> positions;
    std::vector<float> normals;
    std::vector<float> tangents;
    std::vector<float> uv0;
    std::vector<float> uv1;
    std::vector<uint32_t> indices;
    float min[3] = { FLT_MAX, FLT_MAX, FLT_MAX };
    float max[3] = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
};

template<class T>
bool ReadPrimvarValues(const UsdGeomPrimvar& primvar, VtArray<T>& values)
{
    VtArray<T> authored;
    if (!primvar.Get(&authored)) return false;
    if (!primvar.IsIndexed()) {
        values = std::move(authored);
        return true;
    }
    VtIntArray indices;
    if (!primvar.GetIndices(&indices)) return false;
    values.reserve(indices.size());
    for (int index : indices) {
        if (index < 0 || static_cast<size_t>(index) >= authored.size()) return false;
        values.push_back(authored[index]);
    }
    return true;
}

template<class T>
bool SetMeshAttribute(const UsdGeomMesh& mesh, const MeshData& source, const VtArray<T>& values,
    const TfToken& interpolation, PrimvarData<T>& result, bool& requiresRefine)
{
    if (values.empty()) return true;
    if (interpolation == UsdGeomTokens->vertex || interpolation == UsdGeomTokens->varying) {
        if (values.size() != source.points.size()) return false;
        result.values = values;
        result.indices = source.indices;
        return true;
    }

    requiresRefine = true;
    VtIntArray counts, holes;
    if (!mesh.GetFaceVertexCountsAttr().Get(&counts)) return false;
    mesh.GetHoleIndicesAttr().Get(&holes);
    std::unordered_set<int> holeSet(holes.begin(), holes.end());
    if (interpolation == UsdGeomTokens->faceVarying) {
        size_t total = 0;
        for (int count : counts) total += static_cast<size_t>(count);
        if (values.size() != total) return false;
        size_t offset = 0;
        for (size_t face = 0; face < counts.size(); ++face) {
            const size_t count = static_cast<size_t>(counts[face]);
            if (!holeSet.contains(static_cast<int>(face)))
                result.values.insert(result.values.end(), values.begin() + offset, values.begin() + offset + count);
            offset += count;
        }
        return result.values.size() == source.indices.size();
    }
    if (interpolation == UsdGeomTokens->uniform) {
        if (values.size() != counts.size()) return false;
        for (size_t face = 0; face < counts.size(); ++face) {
            if (!holeSet.contains(static_cast<int>(face)))
                result.values.insert(result.values.end(), static_cast<size_t>(counts[face]), values[face]);
        }
        return result.values.size() == source.indices.size();
    }
    if (interpolation == UsdGeomTokens->constant) {
        if (values.size() != 1) return false;
        result.values.assign(source.indices.size(), values[0]);
        return true;
    }
    return false;
}

bool LoadMeshPrimvar(const UsdGeomMesh& mesh, const MeshData& source, const UsdGeomPrimvar& primvar,
    PrimvarData<GfVec2f>& result, bool& requiresRefine)
{
    if (!primvar) return true;
    VtVec2fArray values;
    if (!ReadPrimvarValues(primvar, values) || !SetMeshAttribute(mesh, source, values, primvar.GetInterpolation(), result, requiresRefine)) {
        TF_RUNTIME_ERROR("Invalid mesh primvar '%s' on '%s'", primvar.GetPrimvarName().GetText(), mesh.GetPath().GetText());
        return false;
    }
    return true;
}

bool CollectMesh(const UsdGeomMesh& mesh, UsdGeomXformCache& cache, double metersPerUnit,
    bool zUp, ExportMesh& result)
{
    MeshData source, triangles, refined;
    if (!ExtractMeshData(mesh, source))
        return false;
    if (source.counts.empty())
        return true;

    bool requiresRefine = false;
    VtVec3fArray normals;
    if (!mesh.GetNormalsAttr().Get(&normals) && mesh.GetNormalsAttr().HasAuthoredValueOpinion()) {
        TF_RUNTIME_ERROR("Unable to read mesh normals on '%s'", mesh.GetPath().GetText());
        return false;
    }
    if (!normals.empty() && !SetMeshAttribute(mesh, source, normals, mesh.GetNormalsInterpolation(), source.normals, requiresRefine)) {
        TF_RUNTIME_ERROR("Invalid mesh normals on '%s'", mesh.GetPath().GetText());
        return false;
    }
    UsdGeomPrimvarsAPI primvars(mesh);
    UsdGeomPrimvar tangents = primvars.GetPrimvar(TfToken("tangents"));
    if (tangents) {
        if (tangents.GetTypeName() == SdfValueTypeNames->Float4Array) {
            VtVec4fArray values;
            if (!ReadPrimvarValues(tangents, values) || !SetMeshAttribute(mesh, source, values, tangents.GetInterpolation(), source.tangents, requiresRefine)) {
                TF_RUNTIME_ERROR("Invalid mesh tangents on '%s'", mesh.GetPath().GetText());
                return false;
            }
        }
        else if (tangents.GetTypeName() == SdfValueTypeNames->Vector3fArray) {
            VtVec3fArray values;
            if (!ReadPrimvarValues(tangents, values)) {
                TF_RUNTIME_ERROR("Unable to read mesh tangents on '%s'", mesh.GetPath().GetText());
                return false;
            }
            VtVec4fArray expanded;
            expanded.reserve(values.size());
            for (const GfVec3f& value : values) expanded.push_back(GfVec4f(value[0], value[1], value[2], 1.0f));
            if (!SetMeshAttribute(mesh, source, expanded, tangents.GetInterpolation(), source.tangents, requiresRefine)) {
                TF_RUNTIME_ERROR("Invalid mesh tangents on '%s'", mesh.GetPath().GetText());
                return false;
            }
        }
        else {
            TF_RUNTIME_ERROR("Unsupported tangent type on '%s'", mesh.GetPath().GetText());
            return false;
        }
    }
    UsdGeomPrimvar uv0 = primvars.GetPrimvar(TfToken("st"));
    if (!uv0) uv0 = primvars.GetPrimvar(TfToken("uv"));
    UsdGeomPrimvar uv1 = primvars.GetPrimvar(TfToken("st1"));
    if (!uv1) uv1 = primvars.GetPrimvar(TfToken("uv1"));
    if (!LoadMeshPrimvar(mesh, source, uv0, source.uvs[0], requiresRefine) ||
        !LoadMeshPrimvar(mesh, source, uv1, source.uvs[1], requiresRefine))
        return false;

    if (!TriangulateMesh(source, triangles)) {
        TF_RUNTIME_ERROR("Unable to triangulate mesh '%s'", mesh.GetPath().GetText());
        return false;
    }
    if (requiresRefine && !RefineMesh(triangles, refined)) {
        TF_RUNTIME_ERROR("Unable to refine mesh '%s'", mesh.GetPath().GetText());
        return false;
    }
    if (!requiresRefine) refined = std::move(triangles);
    const auto& points = refined.points;
    if (points.size() > std::numeric_limits<uint32_t>::max()) {
        TF_RUNTIME_ERROR("Mesh '%s' has too many points for glTF", mesh.GetPath().GetText());
        return false;
    }
    const GfMatrix4d transform = cache.GetLocalToWorldTransform(mesh.GetPrim());
    TfToken orientation;
    mesh.GetOrientationAttr().Get(&orientation);
    const bool reverse = (orientation == UsdGeomTokens->leftHanded) != (transform.GetDeterminant3() < 0.0);
    result.name = mesh.GetPrim().GetName().GetString();
    result.positions.reserve(points.size() * 3);
    auto appendVector = [&](std::vector<float>& target, const GfVec3d& vector) {
        GfVec3d value = vector;
        if (zUp) value = GfVec3d(value[0], value[2], -value[1]);
        const double length = value.GetLength();
        if (!std::isfinite(length) || length <= 0.0) return false;
        value /= length;
        for (int axis = 0; axis < 3; ++axis) {
            const float component = static_cast<float>(value[axis]);
            if (!std::isfinite(component)) return false;
            target.push_back(component);
        }
        return true;
    };
    for (const GfVec3f& point : points) {
        GfVec3d p = transform.Transform(GfVec3d(point[0], point[1], point[2])) * metersPerUnit;
        if (zUp)
            p = GfVec3d(p[0], p[2], -p[1]);
        for (int axis = 0; axis < 3; ++axis) {
            const float value = static_cast<float>(p[axis]);
            if (!std::isfinite(value)) {
                TF_RUNTIME_ERROR("Non-finite transformed point on '%s'", mesh.GetPath().GetText());
                return false;
            }
            result.positions.push_back(value);
            result.min[axis] = std::min(result.min[axis], value);
            result.max[axis] = std::max(result.max[axis], value);
        }
    }
    if (!refined.normals.values.empty()) {
        if (refined.normals.values.size() != points.size() || transform.GetDeterminant3() == 0.0) {
            TF_RUNTIME_ERROR("Cannot transform mesh normals through a singular transform on '%s'", mesh.GetPath().GetText());
            return false;
        }
        const GfMatrix4d normalTransform = transform.GetInverse().GetTranspose();
        for (const GfVec3f& value : refined.normals.values) {
            if (!appendVector(result.normals, normalTransform.TransformDir(GfVec3d(value)))) {
                TF_RUNTIME_ERROR("Invalid transformed normal on '%s'", mesh.GetPath().GetText());
                return false;
            }
        }
    }
    if (!refined.tangents.values.empty()) {
        if (refined.tangents.values.size() != points.size()) {
            TF_RUNTIME_ERROR("Mesh tangent count does not match points on '%s'", mesh.GetPath().GetText());
            return false;
        }
        const float handedness = transform.GetDeterminant3() < 0.0 ? -1.0f : 1.0f;
        for (const GfVec4f& value : refined.tangents.values) {
            if ((value[3] != -1.0f && value[3] != 1.0f) ||
                !appendVector(result.tangents, transform.TransformDir(GfVec3d(value[0], value[1], value[2])))) {
                TF_RUNTIME_ERROR("Invalid transformed tangent on '%s'", mesh.GetPath().GetText());
                return false;
            }
            result.tangents.push_back(value[3] * handedness);
        }
    }
    auto appendUv = [&](const VtVec2fArray& values, std::vector<float>& target) {
        if (values.empty()) return true;
        if (values.size() != points.size()) return false;
        for (const GfVec2f& value : values) {
            if (!std::isfinite(value[0]) || !std::isfinite(value[1])) return false;
            target.push_back(value[0]);
            target.push_back(value[1]);
        }
        return true;
    };
    if (!appendUv(refined.uvs[0].values, result.uv0) || !appendUv(refined.uvs[1].values, result.uv1)) {
        TF_RUNTIME_ERROR("Invalid transformed UVs on '%s'", mesh.GetPath().GetText());
        return false;
    }
    result.indices.reserve(refined.indices.size());
    for (size_t i = 0; i < refined.indices.size(); i += 3) {
        result.indices.push_back(static_cast<uint32_t>(refined.indices[i]));
        result.indices.push_back(static_cast<uint32_t>(refined.indices[i + (reverse ? 2 : 1)]));
        result.indices.push_back(static_cast<uint32_t>(refined.indices[i + (reverse ? 1 : 2)]));
    }
    return true;
}

std::string EncodeBase64(const std::vector<uint8_t>& bytes)
{
    static constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string result = "data:application/octet-stream;base64,";
    for (size_t i = 0; i < bytes.size(); i += 3) {
        const uint32_t v = (uint32_t(bytes[i]) << 16)
            | (i + 1 < bytes.size() ? uint32_t(bytes[i + 1]) << 8 : 0)
            | (i + 2 < bytes.size() ? uint32_t(bytes[i + 2]) : 0);
        result.push_back(alphabet[(v >> 18) & 63]);
        result.push_back(alphabet[(v >> 12) & 63]);
        result.push_back(i + 1 < bytes.size() ? alphabet[(v >> 6) & 63] : '=');
        result.push_back(i + 2 < bytes.size() ? alphabet[v & 63] : '=');
    }
    return result;
}

template<class T>
void AppendBytes(std::vector<uint8_t>& bytes, const std::vector<T>& values)
{
    const size_t size = values.size() * sizeof(T);
    const size_t offset = bytes.size();
    bytes.resize(offset + size);
    if (size)
        std::memcpy(bytes.data() + offset, values.data(), size);
}

bool WriteGltf(std::ostream& output, bool glb, std::vector<ExportMesh>& meshes)
{
    const size_t n = meshes.size();
    std::vector<cgltf_mesh> gltfMeshes(n);
    std::vector<cgltf_node> nodes(n);
    std::vector<cgltf_node*> sceneNodes(n);
    std::vector<cgltf_primitive> primitives(n);
    std::vector<cgltf_attribute> attributes;
    std::vector<cgltf_accessor> accessors;
    std::vector<cgltf_buffer_view> views;
    attributes.reserve(n * 5);
    accessors.reserve(n * 6);
    views.reserve(n * 6);
    std::vector<uint8_t> bytes;
    cgltf_buffer buffer{};
    auto appendFloatAccessor = [&](const std::vector<float>& values, cgltf_type type) {
        views.emplace_back();
        cgltf_buffer_view& view = views.back();
        view.buffer = &buffer;
        view.offset = bytes.size();
        AppendBytes(bytes, values);
        view.size = bytes.size() - view.offset;
        accessors.emplace_back();
        cgltf_accessor& accessor = accessors.back();
        accessor.buffer_view = &view;
        accessor.component_type = cgltf_component_type_r_32f;
        accessor.type = type;
        accessor.count = values.size() / cgltf_num_components(type);
        return &accessor;
    };
    for (size_t i = 0; i < n; ++i) {
        ExportMesh& src = meshes[i];
        const size_t firstAttribute = attributes.size();
        cgltf_accessor* position = appendFloatAccessor(src.positions, cgltf_type_vec3);
        position->has_min = position->has_max = true;
        std::copy_n(src.min, 3, position->min);
        std::copy_n(src.max, 3, position->max);
        attributes.emplace_back();
        attributes.back().name = const_cast<char*>("POSITION");
        attributes.back().type = cgltf_attribute_type_position;
        attributes.back().data = position;
        auto appendAttribute = [&](const char* name, cgltf_attribute_type semantic, int index,
            const std::vector<float>& values, cgltf_type type) {
            if (values.empty()) return;
            cgltf_accessor* accessor = appendFloatAccessor(values, type);
            attributes.emplace_back();
            attributes.back().name = const_cast<char*>(name);
            attributes.back().type = semantic;
            attributes.back().index = index;
            attributes.back().data = accessor;
        };
        appendAttribute("NORMAL", cgltf_attribute_type_normal, 0, src.normals, cgltf_type_vec3);
        appendAttribute("TANGENT", cgltf_attribute_type_tangent, 0, src.tangents, cgltf_type_vec4);
        appendAttribute("TEXCOORD_0", cgltf_attribute_type_texcoord, 0, src.uv0, cgltf_type_vec2);
        appendAttribute("TEXCOORD_1", cgltf_attribute_type_texcoord, 1, src.uv1, cgltf_type_vec2);
        const size_t attributeCount = attributes.size() - firstAttribute;

        views.emplace_back();
        cgltf_buffer_view& indicesView = views.back();
        indicesView.buffer = &buffer;
        indicesView.offset = bytes.size();
        AppendBytes(bytes, src.indices);
        indicesView.size = bytes.size() - indicesView.offset;
        accessors.emplace_back();
        cgltf_accessor& index = accessors.back();
        index.buffer_view = &indicesView;
        index.component_type = cgltf_component_type_r_32u;
        index.type = cgltf_type_scalar;
        index.count = src.indices.size();

        primitives[i].type = cgltf_primitive_type_triangles;
        primitives[i].indices = &index;
        primitives[i].attributes = attributes.data() + firstAttribute;
        primitives[i].attributes_count = attributeCount;
        gltfMeshes[i].name = src.name.data();
        gltfMeshes[i].primitives = &primitives[i];
        gltfMeshes[i].primitives_count = 1;
        nodes[i].mesh = &gltfMeshes[i];
        sceneNodes[i] = &nodes[i];
    }
    std::string uri;
    buffer.size = bytes.size();
    if (!glb && !bytes.empty()) {
        uri = EncodeBase64(bytes);
        buffer.uri = uri.data();
    }
    cgltf_scene scene{};
    scene.nodes = sceneNodes.data();
    scene.nodes_count = n;
    cgltf_data data{};
    data.asset.version = const_cast<char*>("2.0");
    data.asset.generator = const_cast<char*>("istGltf");
    data.meshes = gltfMeshes.data(); data.meshes_count = n;
    data.nodes = nodes.data(); data.nodes_count = n;
    data.accessors = accessors.data(); data.accessors_count = accessors.size();
    data.buffer_views = views.data(); data.buffer_views_count = views.size();
    if (n) {
        data.buffers = &buffer; data.buffers_count = 1;
        data.scenes = &scene; data.scenes_count = 1; data.scene = &scene;
    }
    cgltf_options options{};
    const size_t jsonSize = cgltf_write(&options, nullptr, 0, &data);
    std::vector<char> json(jsonSize);
    if (cgltf_write(&options, json.data(), json.size(), &data) != jsonSize) {
        TF_RUNTIME_ERROR("Failed to serialize glTF");
        return false;
    }
    const size_t jsonLength = jsonSize - 1;
    const size_t paddedJson = (jsonLength + 3) & ~size_t(3);
    const size_t total = 12 + 8 + paddedJson + (bytes.empty() ? 0 : 8 + bytes.size());
    if (glb && total > std::numeric_limits<uint32_t>::max()) {
        TF_RUNTIME_ERROR("GLB exceeds 4 GiB");
        return false;
    }
    auto writeU32 = [&](uint32_t value) {
        const char b[] = { char(value), char(value >> 8), char(value >> 16), char(value >> 24) };
        output.write(b, 4);
    };
    if (glb) {
        writeU32(0x46546c67); writeU32(2); writeU32(static_cast<uint32_t>(total));
        writeU32(static_cast<uint32_t>(paddedJson)); writeU32(0x4e4f534a);
    }
    output.write(json.data(), jsonLength);
    if (glb) {
        for (size_t i = jsonLength; i < paddedJson; ++i) output.put(' ');
        if (!bytes.empty()) {
            writeU32(static_cast<uint32_t>(bytes.size())); writeU32(0x004e4942);
            output.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        }
    }
    if (!output) {
        TF_RUNTIME_ERROR("Failed writing glTF output stream");
        return false;
    }
    return true;
}

bool ExportGltf(const SdfLayer& layer, const SdfPath& path, bool glb, std::ostream& output)
{
    SdfLayerHandle root = SdfLayer::Find(layer.GetIdentifier());
    UsdStageRefPtr stage = root ? UsdStage::Open(root) : UsdStageRefPtr();
    if (!stage) {
        TF_RUNTIME_ERROR("Cannot open source layer for glTF export: '%s'", layer.GetIdentifier().c_str());
        return false;
    }
    UsdPrimRange range;
    if (path == SdfPath::AbsoluteRootPath()) {
        range = UsdPrimRange::Stage(stage, UsdTraverseInstanceProxies());
    }
    else {
        UsdPrim prim = stage->GetPrimAtPath(path);
        if (!prim) {
            TF_RUNTIME_ERROR("Cannot find composed prim '%s' for glTF export", path.GetText());
            return false;
        }
        range = UsdPrimRange(prim, UsdTraverseInstanceProxies());
    }
    const double units = UsdGeomGetStageMetersPerUnit(stage);
    if (!std::isfinite(units) || units <= 0.0) {
        TF_RUNTIME_ERROR("Invalid stage metersPerUnit for glTF export");
        return false;
    }
    const bool zUp = UsdGeomGetStageUpAxis(stage) == UsdGeomTokens->z;
    UsdGeomXformCache cache(UsdTimeCode::Default());
    std::vector<ExportMesh> meshes;
    for (const UsdPrim& prim : range) {
        UsdGeomMesh mesh(prim);
        if (!mesh) continue;
        ExportMesh result;
        if (!CollectMesh(mesh, cache, units, zUp, result)) return false;
        if (!result.indices.empty()) meshes.push_back(std::move(result));
    }
    return WriteGltf(output, glb, meshes);
}
}

static bool ValidateGltfBuffers(const cgltf_data& data)
{
    for (size_t i = 0; i < data.buffer_views_count; ++i) {
        const auto& view = data.buffer_views[i];
        if (!view.buffer || !view.buffer->data || view.offset > view.buffer->size || view.size > view.buffer->size - view.offset)
            return false;
    }
    auto fits = [](size_t offset, size_t count, size_t stride, size_t elementSize, size_t size) {
        return count && offset <= size && elementSize <= size - offset &&
            (count - 1) <= (size - offset - elementSize) / stride;
    };
    for (size_t i = 0; i < data.accessors_count; ++i) {
        const auto& accessor = data.accessors[i];
        const size_t elementSize = cgltf_calc_size(accessor.type, accessor.component_type);
        if (!elementSize || !accessor.count) return false;
        if (accessor.buffer_view && (!accessor.stride || !fits(accessor.offset, accessor.count, accessor.stride, elementSize, accessor.buffer_view->size))) return false;
        if (accessor.is_sparse) {
            const auto& sparse = accessor.sparse;
            const size_t indexSize = cgltf_component_size(sparse.indices_component_type);
            if (!sparse.indices_buffer_view || !sparse.values_buffer_view || sparse.count > accessor.count ||
                (sparse.indices_component_type != cgltf_component_type_r_8u && sparse.indices_component_type != cgltf_component_type_r_16u && sparse.indices_component_type != cgltf_component_type_r_32u) ||
                !fits(sparse.indices_byte_offset, sparse.count, indexSize, indexSize, sparse.indices_buffer_view->size) ||
                !fits(sparse.values_byte_offset, sparse.count, elementSize, elementSize, sparse.values_buffer_view->size)) return false;
        }
    }
    return true;
}

static bool ImportGltf(SdfLayer* layer, const std::string& source, bool fromString)
{
    if (!layer) {
        TF_CODING_ERROR("Cannot import glTF into a null layer");
        return false;
    }
    bool detriangulate;
    if (!GetDetriangulate(*layer, detriangulate)) return false;
    cgltf_options options{};
    options.file.read = ReadGltfFile;
    options.file.release = ReleaseGltfFile;
    cgltf_data* parsed = nullptr;
    cgltf_result result = fromString ? cgltf_parse(&options, source.data(), source.size(), &parsed)
        : cgltf_parse_file(&options, source.c_str(), &parsed);
    std::unique_ptr<cgltf_data, decltype(&cgltf_free)> data(parsed, cgltf_free);
    if (result == cgltf_result_success) result = cgltf_load_buffers(&options, data.get(), fromString ? nullptr : source.c_str());
    if (result == cgltf_result_success && !ValidateGltfBuffers(*data)) result = cgltf_result_invalid_gltf;
    if (result == cgltf_result_success) result = cgltf_validate(data.get());
    if (result != cgltf_result_success) {
        TF_RUNTIME_ERROR("Failed to parse or load glTF %s (cgltf error %d)", fromString ? "string" : source.c_str(), static_cast<int>(result));
        return false;
    }
    return TranslateGltf(*data, detriangulate, layer);
}

bool UsdGltfFileFormat::WriteToFile(const SdfLayer& layer, const std::string& path,
    const std::string&, const FileFormatArguments&) const
{
    std::string extension = std::filesystem::u8path(path).extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (extension != ".glb" && extension != ".gltf") {
        TF_RUNTIME_ERROR("Unsupported glTF output extension: '%s'", path.c_str());
        return false;
    }
    std::ostringstream serialized(std::ios::out | std::ios::binary);
    if (!ExportGltf(layer, SdfPath::AbsoluteRootPath(), extension == ".glb", serialized))
        return false;
    std::ofstream output(std::filesystem::u8path(path), std::ios::binary | std::ios::trunc);
    if (!output) {
        TF_RUNTIME_ERROR("Cannot open glTF output '%s'", path.c_str());
        return false;
    }
    const std::string_view data = serialized.view();
    output.write(data.data(), data.size());
    output.close();
    if (!output) {
        TF_RUNTIME_ERROR("Failed writing glTF '%s'", path.c_str());
        return false;
    }
    return true;
}

bool UsdGltfFileFormat::WriteToString(const SdfLayer& layer, std::string* str, const std::string&) const
{
    if (!str) {
        TF_CODING_ERROR("Cannot write glTF to a null string");
        return false;
    }
    std::ostringstream output(std::ios::out | std::ios::binary);
    if (!ExportGltf(layer, SdfPath::AbsoluteRootPath(), GetPrimaryFileExtension() == "glb", output))
        return false;
    *str = std::move(output).str();
    return true;
}

bool UsdGltfFileFormat::WriteToStream(const SdfSpecHandle& spec, std::ostream& out, size_t indent) const
{
    if (!spec || (!spec->GetPath().IsAbsoluteRootPath() && !spec->GetPath().IsPrimPath())) {
        TF_CODING_ERROR("glTF stream export requires a pseudo-root or prim spec");
        return false;
    }
    SdfLayerHandle layer = spec->GetLayer();
    if (!layer) {
        TF_RUNTIME_ERROR("Cannot find source layer for glTF stream export");
        return false;
    }
    const bool glb = GetPrimaryFileExtension() == "glb";
    if (glb && indent != 0) {
        TF_CODING_ERROR("GLB stream export does not support indentation");
        return false;
    }
    std::ostringstream serialized(std::ios::out | std::ios::binary);
    if (!ExportGltf(*layer, spec->GetPath(), glb, serialized))
        return false;
    if (glb) {
        const std::string_view data = serialized.view();
        out.write(data.data(), data.size());
        if (!out) {
            TF_RUNTIME_ERROR("Failed writing GLB output stream");
            return false;
        }
        return true;
    }
    const std::string_view json = serialized.view();
    if (indent > std::numeric_limits<size_t>::max() / 4) {
        TF_CODING_ERROR("glTF stream indentation is too large");
        return false;
    }
    const std::string padding(indent * 4, ' ');
    size_t start = 0;
    while (start < json.size()) {
        const size_t end = json.find('\n', start);
        out.write(padding.data(), padding.size());
        if (end == std::string_view::npos) {
            out.write(json.data() + start, json.size() - start);
            break;
        }
        out.write(json.data() + start, end - start + 1);
        start = end + 1;
    }
    if (!out) {
        TF_RUNTIME_ERROR("Failed writing glTF output stream");
        return false;
    }
    return true;
}

PXR_NAMESPACE_CLOSE_SCOPE
