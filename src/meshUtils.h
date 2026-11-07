#pragma once

#include <memory>

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

    PrimvarData<GfVec3f> normals, binormals, tangents;
    PrimvarData<GfVec4f> colors;
    PrimvarData<GfVec2f> uvs[kMaxUvSets];
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
    class IAttribute
    {
    public:
        virtual ~IAttribute() = default;
        virtual void Prepare(int vertexCount, int indexCount) = 0;
        virtual bool Compare(int vertexIndex, int indexIndex) const = 0;
        virtual void Emit(int indexIndex) = 0;
        virtual void Clear() = 0;
    };

    template<class T>
    class IndexedAttribute;

    template<class T>
    class ExpandedAttribute;

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


// インデックスを作り直す。
// 独立したインデックスを持つ頂点要素やインデックス展開された頂点要素を、単一のインデックスに統合する。
// これに伴い、頂点数は増える場合がある。
bool RefineMesh(const MeshData& src, MeshData& dst);

// 三角形化。line / point は無視される
bool TriangulateMesh(const MeshData& src, MeshData& dst);
