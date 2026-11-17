#include "pxr/base/plug/registry.h"
#include "pxr/usd/sdf/fileFormat.h"
#include "pxr/usd/sdf/layer.h"
#include "pxr/usd/sdf/primSpec.h"
#include "pxr/usd/sdf/spec.h"
#include "pxr/usd/usd/stage.h"
#include "pxr/usd/usdGeom/mesh.h"
#include "pxr/usd/usdGeom/metrics.h"
#include "pxr/usd/usdGeom/xform.h"

#define CGLTF_IMPLEMENTATION
#include <cgltf.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>

PXR_NAMESPACE_USING_DIRECTIVE

static bool Check(bool value, const char* message)
{
    if (!value) std::cerr << message << '\n';
    return value;
}

static bool Verify(const std::filesystem::path& path, float expectedArea = -0.0006f)
{
    cgltf_options options{};
    cgltf_data* data = nullptr;
    const std::string filename = path.string();
    if (!Check(cgltf_parse_file(&options, filename.c_str(), &data) == cgltf_result_success, "parse failed")) return false;
    bool ok = Check(cgltf_load_buffers(&options, data, filename.c_str()) == cgltf_result_success, "buffer load failed")
        && Check(cgltf_validate(data) == cgltf_result_success, "glTF validation failed")
        && Check(data->meshes_count == 1 && data->nodes_count == 1, "unexpected scene structure");
    if (ok) {
        const cgltf_primitive& primitive = data->meshes[0].primitives[0];
        const cgltf_accessor* position = cgltf_find_accessor(&primitive, cgltf_attribute_type_position, 0);
        float point[3]{};
        ok = Check(position && position->count == 5 && primitive.indices->count == 9, "incorrect geometry counts")
            && Check(cgltf_accessor_read_float(position, 0, point, 3), "position read failed")
            && Check(std::abs(point[0] - 1.0f) < 1e-5f && std::abs(point[1] - 3.0f) < 1e-5f
                && std::abs(point[2] + 2.0f) < 1e-5f, "transform or units incorrect");
        if (ok) {
            float signedArea = 0;
            for (size_t i = 0; i < primitive.indices->count; i += 3) {
                float a[3], b[3], c[3];
                cgltf_accessor_read_float(position, cgltf_accessor_read_index(primitive.indices, i), a, 3);
                cgltf_accessor_read_float(position, cgltf_accessor_read_index(primitive.indices, i + 1), b, 3);
                cgltf_accessor_read_float(position, cgltf_accessor_read_index(primitive.indices, i + 2), c, 3);
                signedArea += (b[0] - a[0]) * (c[2] - a[2]) - (b[2] - a[2]) * (c[0] - a[0]);
            }
            ok = Check(std::abs(signedArea - expectedArea) < 1e-7f, "concave triangulation or winding incorrect");
        }
    }
    cgltf_free(data);
    return ok;
}

static bool VerifyText(const std::string& text, size_t meshCount)
{
    cgltf_options options{};
    cgltf_data* data = nullptr;
    if (!Check(cgltf_parse(&options, text.data(), text.size(), &data) == cgltf_result_success, "text parse failed")) return false;
    const bool ok = Check(cgltf_load_buffers(&options, data, nullptr) == cgltf_result_success, "text buffer load failed")
        && Check(cgltf_validate(data) == cgltf_result_success, "text validation failed")
        && Check(data->meshes_count == meshCount, "text mesh count incorrect");
    cgltf_free(data);
    return ok;
}

static bool VerifyImported(const SdfLayerRefPtr& layer, const VtIntArray& expectedCounts)
{
    if (!Check(static_cast<bool>(layer), "imported layer missing")) return false;
    UsdStageRefPtr stage = UsdStage::Open(layer);
    if (!Check(static_cast<bool>(stage), "imported stage missing")) return false;
    UsdGeomMesh mesh(stage->GetPrimAtPath(SdfPath("/Mesh_0")));
    VtVec3fArray points;
    VtIntArray counts;
    return Check(mesh && mesh.GetPointsAttr().Get(&points) && mesh.GetFaceVertexCountsAttr().Get(&counts), "imported mesh missing")
        && Check(counts == expectedCounts, "imported face counts incorrect")
        && Check(points.size() == 5 && std::abs(points[0][0] - 1.0f) < 1e-5f
            && std::abs(points[0][1] - 3.0f) < 1e-5f && std::abs(points[0][2] + 2.0f) < 1e-5f, "imported positions incorrect")
        && Check(UsdGeomGetStageUpAxis(stage) == UsdGeomTokens->y && UsdGeomGetStageMetersPerUnit(stage) == 1.0, "imported stage metrics incorrect");
}

static bool TestImportScene(const SdfFileFormatConstPtr& format, const std::filesystem::path& directory)
{
    const std::string encoded = "AAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAQAAAAEAAAAAAAACAPwAAgD8AAAAAAAAAAAAAAEAAAAAAAAABAAIAAAACAAMAAAADAAQA";
    const std::string prefix = R"({"asset":{"version":"2.0"},"buffers":[{"byteLength":78,"uri":")";
    const std::string suffix = R"("}],"bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":60},{"buffer":0,"byteOffset":60,"byteLength":18}],"accessors":[{"bufferView":0,"componentType":5126,"count":5,"type":"VEC3","min":[0,0,0],"max":[2,2,0]},{"bufferView":1,"componentType":5123,"count":9,"type":"SCALAR"}],"meshes":[{"primitives":[{"attributes":{"POSITION":0},"indices":1},{"attributes":{"POSITION":0},"mode":5},{"attributes":{"POSITION":0},"mode":6}]}],"nodes":[{"translation":[10,20,30],"children":[1]},{"scale":[-2,1,1],"mesh":0},{"translation":[1,2,3],"mesh":0},{"mesh":0}],"scenes":[{"nodes":[0,2]},{"nodes":[3]}],"scene":0})";
    const std::string text = prefix + "data:application/octet-stream;base64," + encoded + suffix;
    SdfLayerRefPtr layer = SdfLayer::CreateAnonymous("import.usda");
    if (!Check(format->ReadFromString(layer.operator->(), text), "scene string import failed")) return false;
    UsdStageRefPtr stage = UsdStage::Open(layer);
    UsdGeomMesh first(stage->GetPrimAtPath(SdfPath("/Mesh_0")));
    UsdGeomMesh strip(stage->GetPrimAtPath(SdfPath("/Mesh_1")));
    UsdGeomMesh fan(stage->GetPrimAtPath(SdfPath("/Mesh_2")));
    UsdGeomMesh instance(stage->GetPrimAtPath(SdfPath("/Mesh_3")));
    VtVec3fArray points;
    VtIntArray indices;
    bool ok = Check(first && strip && fan && instance && stage->GetPrimAtPath(SdfPath("/Mesh_5"))
        && !stage->GetPrimAtPath(SdfPath("/Mesh_6")), "scene selection or instances incorrect")
        && Check(first.GetPointsAttr().Get(&points) && points[0] == GfVec3f(10,20,30) && points[1] == GfVec3f(6,20,30), "nested transform incorrect")
        && Check(first.GetFaceVertexIndicesAttr().Get(&indices) && indices[0] == 0 && indices[1] == 2 && indices[2] == 1, "mirrored winding incorrect")
        && Check(strip.GetFaceVertexIndicesAttr().Get(&indices) && indices == VtIntArray{0,2,1,2,3,1,2,4,3}, "triangle strip incorrect")
        && Check(fan.GetFaceVertexIndicesAttr().Get(&indices) && indices == VtIntArray{0,2,1,0,3,2,0,4,3}, "triangle fan incorrect")
        && Check(instance.GetPointsAttr().Get(&points) && points[0] == GfVec3f(1,2,3), "shared mesh transform incorrect");
    if (!ok) return false;
    const std::string sparse = R"({"asset":{"version":"2.0"},"buffers":[{"byteLength":48,"uri":"data:application/octet-stream;base64,AAECAAAAAAAAAAAAAAAAAAAAgD8AAAAAAAAAAAAAAAAAAIA/AAAAAAECAAABAAIA"}],"bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":3},{"buffer":0,"byteOffset":4,"byteLength":36},{"buffer":0,"byteOffset":40,"byteLength":2},{"buffer":0,"byteOffset":44,"byteLength":4}],"accessors":[{"componentType":5126,"count":3,"type":"VEC3","min":[0,0,0],"max":[1,1,0],"sparse":{"count":3,"indices":{"bufferView":0,"componentType":5121},"values":{"bufferView":1}}},{"componentType":5123,"count":3,"type":"SCALAR","sparse":{"count":2,"indices":{"bufferView":2,"componentType":5121},"values":{"bufferView":3}}}],"meshes":[{"primitives":[{"attributes":{"POSITION":0},"indices":1}]}],"nodes":[{"mesh":0}]})";
    SdfLayerRefPtr sparseLayer = SdfLayer::CreateAnonymous("sparse.usda");
    if (!Check(format->ReadFromString(sparseLayer.operator->(), sparse), "sparse import failed")) return false;
    UsdStageRefPtr sparseStage = UsdStage::Open(sparseLayer);
    UsdGeomMesh sparseMesh(sparseStage->GetPrimAtPath(SdfPath("/Mesh_0")));
    if (!Check(sparseMesh && sparseMesh.GetPointsAttr().Get(&points) && points == VtVec3fArray{GfVec3f(0,0,0),GfVec3f(1,0,0),GfVec3f(0,1,0)}
        && sparseMesh.GetFaceVertexIndicesAttr().Get(&indices) && indices == VtIntArray{0,1,2}, "sparse geometry incorrect")) return false;
    if (!Check(!format->ReadFromString(sparseLayer.operator->(), "{ invalid"), "invalid JSON accepted")
        || !Check(sparseMesh.GetFaceVertexIndicesAttr().Get(&indices) && indices == VtIntArray{0,1,2}, "failed import modified layer")) return false;
    SdfLayerRefPtr invalid = SdfLayer::CreateAnonymous("invalid.usda", {{"detriangulate", "maybe"}});
    if (!Check(!format->ReadFromString(invalid.operator->(), text), "invalid detriangulate accepted")) return false;
    if (!Check(!format->ReadFromString(layer.operator->(), prefix + "istGltf_external_test.bin" + suffix), "relative buffer accepted from string")) return false;
    const auto gltf = directory / "istGltf_external_test.gltf";
    const auto bin = directory / "istGltf_external_test.bin";
    if (!Check(!std::filesystem::exists(gltf) && !std::filesystem::exists(bin), "external test files already exist")) return false;
    const float positions[] = {0,0,0, 2,0,0, 2,2,0, 1,1,0, 0,2,0};
    const uint16_t faces[] = {0,1,2, 0,2,3, 0,3,4};
    {
        std::ofstream out(bin, std::ios::binary);
        out.write(reinterpret_cast<const char*>(positions), sizeof(positions));
        out.write(reinterpret_cast<const char*>(faces), sizeof(faces));
        if (!Check(out.good(), "external buffer write failed")) return false;
    }
    {
        std::ofstream out(gltf, std::ios::binary);
        out << prefix << "istGltf_external_test.bin" << suffix;
        if (!Check(out.good(), "external gltf write failed")) return false;
    }
    SdfLayerRefPtr external = SdfLayer::FindOrOpen(gltf.string());
    ok = Check(static_cast<bool>(external), "external buffer import failed");
    if (ok) {
        UsdStageRefPtr externalStage = UsdStage::Open(external);
        UsdGeomMesh externalMesh(externalStage->GetPrimAtPath(SdfPath("/Mesh_0")));
        ok = Check(externalMesh && externalMesh.GetPointsAttr().Get(&points) && points[1] == GfVec3f(6,20,30), "external buffer geometry incorrect");
    }
    std::filesystem::remove(gltf);
    std::filesystem::remove(bin);
    return ok;
}

int main(int argc, char** argv)
{
    if (argc != 3) return 2;
    PlugRegistry::GetInstance().RegisterPlugins(argv[1]);
    if (!Check(SdfFileFormat::FindByExtension("gltf") && SdfFileFormat::FindByExtension("glb"), "plugin registration failed")) return 1;
    UsdStageRefPtr stage = UsdStage::CreateInMemory();
    UsdGeomSetStageUpAxis(stage, UsdGeomTokens->z);
    UsdGeomSetStageMetersPerUnit(stage, 0.01);
    UsdGeomXform parent = UsdGeomXform::Define(stage, SdfPath("/Parent"));
    parent.AddTranslateOp().Set(GfVec3d(100, 200, 300));
    UsdGeomMesh mesh = UsdGeomMesh::Define(stage, SdfPath("/Parent/Mesh"));
    mesh.CreatePointsAttr().Set(VtVec3fArray{ GfVec3f(0,0,0), GfVec3f(2,0,0), GfVec3f(2,2,0), GfVec3f(1,1,0), GfVec3f(0,2,0) });
    mesh.CreateFaceVertexCountsAttr().Set(VtIntArray{ 5, 3 });
    mesh.CreateFaceVertexIndicesAttr().Set(VtIntArray{ 0,1,2,3,4, 0,1,2 });
    mesh.CreateHoleIndicesAttr().Set(VtIntArray{ 1 });
    const std::filesystem::path directory(argv[2]);
    const auto gltf = directory / "istGltf_test.gltf";
    const auto glb = directory / "istGltf_test.glb";
    if (!Check(!std::filesystem::exists(gltf) && !std::filesystem::exists(glb), "test output already exists")) return 1;
    bool ok = Check(stage->GetRootLayer()->Export(gltf.string()), "gltf export failed") && Verify(gltf)
        && Check(stage->GetRootLayer()->Export(glb.string()), "glb export failed") && Verify(glb);
    const auto format = SdfFileFormat::FindByExtension("gltf");
    std::string text;
    if (ok) ok = TestImportScene(format, directory);
    if (ok) {
        SdfLayerRefPtr gltfLayer = SdfLayer::FindOrOpen(gltf.string());
        SdfLayerRefPtr glbLayer = SdfLayer::FindOrOpen(glb.string());
        SdfLayerRefPtr restored = SdfLayer::FindOrOpen(gltf.string(), {{"detriangulate", "true"}});
        SdfLayerRefPtr restoredGlb = SdfLayer::FindOrOpen(glb.string(), {{"detriangulate", "1"}});
        SdfLayerRefPtr triangles = SdfLayer::FindOrOpen(gltf.string(), {{"detriangulate", "0"}});
        ok = VerifyImported(gltfLayer, VtIntArray{3, 3, 3})
            && VerifyImported(glbLayer, VtIntArray{3, 3, 3})
            && VerifyImported(restored, VtIntArray{5})
            && VerifyImported(restoredGlb, VtIntArray{5})
            && VerifyImported(triangles, VtIntArray{3, 3, 3});
    }
    if (ok) {
        std::ifstream input(gltf, std::ios::binary);
        const std::string fileText((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        std::ostringstream stream;
        ok = Check(format->WriteToString(*stage->GetRootLayer(), &text), "string export failed")
            && Check(text == fileText, "string differs from file export") && VerifyText(text, 1)
            && Check(format->WriteToStream(stage->GetRootLayer()->GetPseudoRoot(), stream, 0), "root stream export failed")
            && Check(stream.str() == text, "root stream differs from string export");
        SdfLayerRefPtr imported = SdfLayer::CreateAnonymous("import.usda", {{"detriangulate", "true"}});
        if (ok) ok = Check(format->ReadFromString(imported.operator->(), text), "string import failed")
            && VerifyImported(imported, VtIntArray{5});
    }
    if (ok) {
        UsdGeomMesh other = UsdGeomMesh::Define(stage, SdfPath("/Other"));
        other.CreatePointsAttr().Set(VtVec3fArray{ GfVec3f(0,0,0), GfVec3f(1,0,0), GfVec3f(0,1,0) });
        other.CreateFaceVertexCountsAttr().Set(VtIntArray{ 3 });
        other.CreateFaceVertexIndicesAttr().Set(VtIntArray{ 0,1,2 });
        std::ostringstream stream;
        ok = Check(format->WriteToString(*stage->GetRootLayer(), &text) && VerifyText(text, 2), "multi-mesh string export failed")
            && Check(format->WriteToStream(stage->GetRootLayer()->GetPrimAtPath(SdfPath("/Parent")), stream, 1), "prim stream export failed")
            && Check(stream.str().starts_with("    {"), "stream indentation incorrect")
            && VerifyText(stream.str(), 1);
        if (ok) {
            cgltf_options options{};
            cgltf_data* data = nullptr;
            const std::string subTree = stream.str();
            ok = Check(cgltf_parse(&options, subTree.data(), subTree.size(), &data) == cgltf_result_success, "subtree parse failed");
            if (ok) {
                float point[3]{};
                const auto* position = cgltf_find_accessor(&data->meshes[0].primitives[0], cgltf_attribute_type_position, 0);
                ok = Check(cgltf_load_buffers(&options, data, nullptr) == cgltf_result_success && position
                    && cgltf_accessor_read_float(position, 0, point, 3) && std::abs(point[0] - 1.0f) < 1e-5f
                    && std::abs(point[1] - 3.0f) < 1e-5f && std::abs(point[2] + 2.0f) < 1e-5f,
                    "subtree transform incorrect");
            }
            if (data) cgltf_free(data);
        }
        stage->RemovePrim(SdfPath("/Other"));
    }
    if (ok) {
        std::ostringstream failed;
        failed.setstate(std::ios::badbit);
        ok = Check(!format->WriteToStream(stage->GetRootLayer()->GetPseudoRoot(), failed, 0), "failed stream accepted")
            && Check(!format->WriteToString(*stage->GetRootLayer(), nullptr), "null output accepted")
            && Check(!format->WriteToStream(stage->GetRootLayer()->GetObjectAtPath(SdfPath("/Parent/Mesh.points")), failed, 0), "property spec accepted");
    }
    if (ok) {
        mesh.AddScaleOp().Set(GfVec3f(-1, 1, 1));
        ok = Check(stage->GetRootLayer()->Export(glb.string()), "mirrored export failed") && Verify(glb);
    }
    if (ok) {
        mesh.CreateOrientationAttr().Set(UsdGeomTokens->leftHanded);
        ok = Check(stage->GetRootLayer()->Export(glb.string()), "left-handed export failed") && Verify(glb, 0.0006f);
    }
    if (ok) {
        mesh.GetFaceVertexIndicesAttr().Set(VtIntArray{ 0,1,99,3,4, 0,1,2 });
        text = "unchanged";
        std::ostringstream stream;
        stream << "unchanged";
        ok = Check(!stage->GetRootLayer()->Export(glb.string()), "invalid index was accepted")
            && Check(!format->WriteToString(*stage->GetRootLayer(), &text) && text == "unchanged", "invalid string export modified output")
            && Check(!format->WriteToStream(stage->GetRootLayer()->GetPseudoRoot(), stream, 0) && stream.str() == "unchanged", "invalid stream export modified output");
    }
    if (ok) {
        UsdStageRefPtr empty = UsdStage::CreateInMemory();
        std::ostringstream stream;
        ok = Check(format->WriteToString(*empty->GetRootLayer(), &text) && VerifyText(text, 0), "empty string export failed")
            && Check(format->WriteToStream(empty->GetRootLayer()->GetPseudoRoot(), stream, 0) && stream.str() == text, "empty stream export failed")
            && Check(empty->GetRootLayer()->Export(glb.string()), "empty scene export failed");
        cgltf_options options{};
        cgltf_data* data = nullptr;
        if (ok) ok = Check(cgltf_parse_file(&options, glb.string().c_str(), &data) == cgltf_result_success, "empty scene parse failed");
        if (ok) ok = Check(cgltf_validate(data) == cgltf_result_success && data->meshes_count == 0, "empty scene invalid");
        if (data) cgltf_free(data);
    }
    std::filesystem::remove(gltf);
    std::filesystem::remove(glb);
    return ok ? 0 : 1;
}
