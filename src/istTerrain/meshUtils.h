#pragma once

#include <pxr/base/gf/matrix4f.h>
#include <pxr/base/gf/range3f.h>
#include <pxr/usd/usdGeom/mesh.h>

#include <cstdint>
#include <memory>
#include <span>

// Requires AVX2. Empty input returns an empty range; NaN components are ignored.
GfRange3f FastBounds(std::span<const GfVec3f> points);

// Requires AVX2. Output must hold at least input.size() elements; exact input/output aliasing is supported.
void NormalizeVectors(std::span<GfVec3f> output, std::span<const GfVec3f> input);

// Requires AVX2. Inputs must have equal sizes and output must hold them; exact aliasing with either input is supported.
void Lerp(std::span<float> output, std::span<const float> a, std::span<const float> b, float t);
void Lerp(std::span<GfVec2f> output, std::span<const GfVec2f> a, std::span<const GfVec2f> b, float t);
void Lerp(std::span<GfVec3f> output, std::span<const GfVec3f> a, std::span<const GfVec3f> b, float t);

// Both require AVX2. Output must hold at least input.size() elements; exact input/output aliasing is supported.
// Matches GfMatrix4f::Transform, including homogeneous projection (no division for w == 0).
void TransformPoints(std::span<GfVec3f> output, std::span<const GfVec3f> input, const GfMatrix4f& transform);
// Matches GfMatrix4f::TransformDir: no translation or homogeneous projection.
void TransformVectors(std::span<GfVec3f> output, std::span<const GfVec3f> input, const GfMatrix4f& transform);

// Requires AVX2. output.size() defines the vertex count; the attribute spans must cover it.
// indices is a triangle list with three vertex indices per triangle.
void GenerateTangentsTriangleIndexed(std::span<GfVec4f> output,
    std::span<const GfVec3f> vertices, std::span<const GfVec2f> uvs,
    std::span<const GfVec3f> normals, std::span<const int> indices);
void GenerateTangentsTriangleIndexed(std::span<GfVec3f> output,
    std::span<const GfVec3f> vertices, std::span<const GfVec2f> uvs,
    std::span<const GfVec3f> normals, std::span<const int> indices);


struct RGu8 { uint8_t r, g; };
struct RGBu8 { uint8_t r, g, b; };
struct RGBAu8 { uint8_t r, g, b, a; };
static_assert(sizeof(RGu8) == 2 && sizeof(RGBu8) == 3 && sizeof(RGBAu8) == 4);

// Requires AVX2. Reconstruct RGB UNORM8 from RG or RGBA (G = X, A = Y) UNORM8 pixels.
// Z = sqrt(max(0, 1 - X*X - Y*Y)), encoded as UNORM8 by truncation.
// Output must hold input.size() pixels. Equal starting addresses are supported; other overlaps are not.
// Insufficient output causes a runtime diagnostic and leaves output unchanged.
void ReconstructNormalMapRG8(std::span<RGBu8> output, std::span<const RGu8> input);
void ReconstructNormalMapGA8(std::span<RGBu8> output, std::span<const RGBAu8> input);


constexpr int kMaxUvSets = 4;

template<typename T>
struct PrimvarData
{
    VtArray<int> indices;
    VtArray<T> values;
};

struct MeshData
{
    VtArray<int> indices;
    VtArray<int> counts;
    VtArray<GfVec3f> points;

    PrimvarData<GfVec3f> normals, binormals;
    PrimvarData<GfVec4f> tangents, colors;
    PrimvarData<GfVec2f> uvs[kMaxUvSets];

    VtArray<int> jointIndices;
    VtArray<float> jointWeights;
    int jointsPerVertex = 0;
};

#define EachMeshPrimvar(Action) \
    Action(normals) \
    Action(binormals) \
    Action(tangents) \
    Action(colors) \
    Action(uvs[0]) \
    Action(uvs[1]) \
    Action(uvs[2]) \
    Action(uvs[3]) \


class MeshRefiner
{
public:
    VtArray<int> counts;
    VtArray<int> indices;
    VtArray<GfVec3f> points;

    VtArray<int> old2newIndices;
    VtArray<int> new2oldPoints;
    VtArray<int> newCounts;
    VtArray<int> newIndices;
    VtArray<GfVec3f> newPoints;

    template<class T>
    void AddIndexedAttribute(const VtArray<T>& values, const VtArray<int>& attrIndices, VtArray<T>& newValues, VtArray<int>& new2old);

    template<class T>
    void AddExpandedAttribute(const VtArray<T>& values, VtArray<T>& newValues, VtArray<int>& new2old);

    void Refine();
    void Clear();

private:
    class IAttribute;
    template<class T> class IndexedAttribute;
    template<class T> class ExpandedAttribute;

    std::vector<std::unique_ptr<IAttribute>> attributes;
};

class Triangulator
{
public:
    using float2 = GfVec2f;
    using float3 = GfVec3f;

    bool EarClipTriangulate(std::span<const float3> points, std::span<const int> indices);
    bool SimpleTriangulate(std::span<const int> indices);
    std::span<const int> GetResult() const { return result; }

private:
    bool DoEarClipTriangulate();

    std::vector<int> polygon;
    std::vector<int> result;
    std::vector<float2> polygon2DVertices;
};

class Detriangulator
{
public:
    // 三角形 indices の連続した出力順から、復元可能なポリゴンをまとめる。
    VtArray<int> counts;
    VtArray<int> indices;
    VtArray<int> new2oldIndices;

    bool Detriangulate(std::span<const int> triangleIndices);
    void Clear();
};


// Extract default-time positions and topology, validating indices and omitting hole faces.
bool ExtractMeshData(const UsdGeomMesh& mesh, MeshData& dst);

// インデックスを作り直す。
// 独立したインデックスを持つ頂点要素やインデックス展開された頂点要素を、単一のインデックスに統合する。
// これに伴い、頂点数は増える場合がある。
bool RefineMesh(const MeshData& src, MeshData& dst);

// 三角形化。line / point は無視される
bool TriangulateMesh(const MeshData& src, MeshData& dst);

// 三角形化された mesh を可能な範囲で多角形に戻す。
bool DetriangulateMesh(const MeshData& src, MeshData& dst);

// 頂点あたりの joint 数を変更する。
// 減らす場合は影響度の小さいものを切り捨てて正規化し、増やす場合は 0 で埋める。
bool SetJointsPerVertex(MeshData& dst, int n);
