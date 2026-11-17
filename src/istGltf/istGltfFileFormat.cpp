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

bool ReadPositions(const cgltf_accessor& accessor, std::vector<float>& values)
{
    if (accessor.type != cgltf_type_vec3 || accessor.component_type != cgltf_component_type_r_32f ||
        accessor.count == 0 || accessor.count > static_cast<size_t>(std::numeric_limits<int>::max()) ||
        (accessor.buffer_view && accessor.buffer_view->has_meshopt_compression)) return false;
    values.resize(accessor.count * 3);
    cgltf_accessor base = accessor;
    base.is_sparse = false;
    if (cgltf_accessor_unpack_floats(&base, values.data(), values.size()) != values.size()) return false;
    if (accessor.is_sparse) {
        const auto& sparse = accessor.sparse;
        if (!sparse.indices_buffer_view || !sparse.values_buffer_view || sparse.indices_buffer_view->has_meshopt_compression || sparse.values_buffer_view->has_meshopt_compression) return false;
        const uint8_t* indices = cgltf_buffer_view_data(sparse.indices_buffer_view);
        const uint8_t* data = cgltf_buffer_view_data(sparse.values_buffer_view);
        if (!indices || !data) return false;
        indices += sparse.indices_byte_offset;
        data += sparse.values_byte_offset;
        const size_t stride = cgltf_component_size(sparse.indices_component_type);
        for (size_t i = 0; i < sparse.count; ++i) {
            const uint32_t index = ReadUnsigned(indices + i * stride, sparse.indices_component_type);
            if (index >= accessor.count) return false;
            std::memcpy(values.data() + index * 3, data + i * 3 * sizeof(float), 3 * sizeof(float));
        }
    }
    return true;
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
    std::vector<uint32_t> indices;
    float min[3] = { FLT_MAX, FLT_MAX, FLT_MAX };
    float max[3] = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
};

bool CollectMesh(const UsdGeomMesh& mesh, UsdGeomXformCache& cache, double metersPerUnit,
    bool zUp, ExportMesh& result)
{
    MeshData source, triangles;
    if (!ExtractMeshData(mesh, source))
        return false;
    if (source.counts.empty())
        return true;
    if (!TriangulateMesh(source, triangles)) {
        TF_RUNTIME_ERROR("Unable to triangulate mesh '%s'", mesh.GetPath().GetText());
        return false;
    }
    const auto& points = triangles.points;
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
    result.indices.reserve(triangles.indices.size());
    for (size_t i = 0; i < triangles.indices.size(); i += 3) {
        result.indices.push_back(static_cast<uint32_t>(triangles.indices[i]));
        result.indices.push_back(static_cast<uint32_t>(triangles.indices[i + (reverse ? 2 : 1)]));
        result.indices.push_back(static_cast<uint32_t>(triangles.indices[i + (reverse ? 1 : 2)]));
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
    std::vector<cgltf_attribute> attributes(n);
    std::vector<cgltf_accessor> accessors(n * 2);
    std::vector<cgltf_buffer_view> views(n * 2);
    std::vector<uint8_t> bytes;
    cgltf_buffer buffer{};
    for (size_t i = 0; i < n; ++i) {
        ExportMesh& src = meshes[i];
        cgltf_buffer_view& positionsView = views[i * 2];
        positionsView.buffer = &buffer;
        positionsView.offset = bytes.size();
        AppendBytes(bytes, src.positions);
        positionsView.size = bytes.size() - positionsView.offset;
        cgltf_buffer_view& indicesView = views[i * 2 + 1];
        indicesView.buffer = &buffer;
        indicesView.offset = bytes.size();
        AppendBytes(bytes, src.indices);
        indicesView.size = bytes.size() - indicesView.offset;

        cgltf_accessor& position = accessors[i * 2];
        position.buffer_view = &positionsView;
        position.component_type = cgltf_component_type_r_32f;
        position.type = cgltf_type_vec3;
        position.count = src.positions.size() / 3;
        position.has_min = position.has_max = true;
        std::copy_n(src.min, 3, position.min);
        std::copy_n(src.max, 3, position.max);
        cgltf_accessor& index = accessors[i * 2 + 1];
        index.buffer_view = &indicesView;
        index.component_type = cgltf_component_type_r_32u;
        index.type = cgltf_type_scalar;
        index.count = src.indices.size();
        attributes[i].name = const_cast<char*>("POSITION");
        attributes[i].data = &position;
        primitives[i].type = cgltf_primitive_type_triangles;
        primitives[i].indices = &index;
        primitives[i].attributes = &attributes[i];
        primitives[i].attributes_count = 1;
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
