#include "pch.h"
#include "meshUtils.h"

#include <algorithm>
#include <unordered_set>

using float2 = GfVec2f;
using float3 = GfVec3f;

template<class T>
class MeshRefiner::IndexedAttribute final : public MeshRefiner::IAttribute
{
public:
    void Prepare(int /*vertexCount*/, int /*indexCount*/) override
    {
        Clear();
    }

    bool Compare(int vertexIndex, int indexIndex) const override
    {
        return (*newValues)[vertexIndex] == (*values)[(*indices)[indexIndex]];
    }

    void Emit(int indexIndex) override
    {
        const int srcIndex = (*indices)[indexIndex];
        newValues->push_back((*values)[srcIndex]);
        new2old->push_back(srcIndex);
    }

    void Clear() override
    {
        newValues->clear();
        new2old->clear();
    }

    const VtArray<T>* values = nullptr;
    const VtArray<int>* indices = nullptr;
    VtArray<T>* newValues = nullptr;
    VtArray<int>* new2old = nullptr;
};

template<class T>
class MeshRefiner::ExpandedAttribute final : public MeshRefiner::IAttribute
{
public:
    void Prepare(int /*vertexCount*/, int /*indexCount*/) override
    {
        Clear();
    }

    bool Compare(int vertexIndex, int indexIndex) const override
    {
        return (*newValues)[vertexIndex] == (*values)[indexIndex];
    }

    void Emit(int indexIndex) override
    {
        newValues->push_back((*values)[indexIndex]);
        new2old->push_back(indexIndex);
    }

    void Clear() override
    {
        newValues->clear();
        new2old->clear();
    }

    const VtArray<T>* values = nullptr;
    VtArray<T>* newValues = nullptr;
    VtArray<int>* new2old = nullptr;
};

template<class T>
void MeshRefiner::AddIndexedAttribute(const VtArray<T>& values, const VtArray<int>& attrIndices, VtArray<T>& newValues, VtArray<int>& new2old)
{
    auto attr = std::make_unique<IndexedAttribute<T>>();
    attr->values = &values;
    attr->indices = &attrIndices;
    attr->newValues = &newValues;
    attr->new2old = &new2old;
    attributes.push_back(std::move(attr));
}

template<class T>
void MeshRefiner::AddExpandedAttribute(const VtArray<T>& values, VtArray<T>& newValues, VtArray<int>& new2old)
{
    auto attr = std::make_unique<ExpandedAttribute<T>>();
    attr->values = &values;
    attr->newValues = &newValues;
    attr->new2old = &new2old;
    attributes.push_back(std::move(attr));
}

template void MeshRefiner::AddIndexedAttribute<GfVec3f>(const VtArray<GfVec3f>& values, const VtArray<int>& attrIndices, VtArray<GfVec3f>& newValues, VtArray<int>& new2old);
template void MeshRefiner::AddIndexedAttribute<GfVec4f>(const VtArray<GfVec4f>& values, const VtArray<int>& attrIndices, VtArray<GfVec4f>& newValues, VtArray<int>& new2old);
template void MeshRefiner::AddIndexedAttribute<GfVec2f>(const VtArray<GfVec2f>& values, const VtArray<int>& attrIndices, VtArray<GfVec2f>& newValues, VtArray<int>& new2old);

template void MeshRefiner::AddExpandedAttribute<GfVec3f>(const VtArray<GfVec3f>& values, VtArray<GfVec3f>& newValues, VtArray<int>& new2old);
template void MeshRefiner::AddExpandedAttribute<GfVec4f>(const VtArray<GfVec4f>& values, VtArray<GfVec4f>& newValues, VtArray<int>& new2old);
template void MeshRefiner::AddExpandedAttribute<GfVec2f>(const VtArray<GfVec2f>& values, VtArray<GfVec2f>& newValues, VtArray<int>& new2old);

void MeshRefiner::Clear()
{
    counts.clear();
    indices.clear();
    points.clear();

    old2newIndices.clear();
    new2oldPoints.clear();
    newCounts.clear();
    newIndices.clear();
    newPoints.clear();

    for (auto& attribute : attributes) {
        attribute->Clear();
    }
    attributes.clear();
}

void MeshRefiner::Refine()
{
    newCounts.clear();
    newIndices.clear();
    newPoints.clear();
    new2oldPoints.clear();

    for (auto& attribute : attributes) {
        attribute->Prepare(static_cast<int>(points.size()), static_cast<int>(indices.size()));
    }

    VtArray<int> sourceIndices = indices;
    if (sourceIndices.empty()) {
        sourceIndices.resize(points.size());
        std::iota(sourceIndices.begin(), sourceIndices.end(), 0);
    }

    old2newIndices.resize(sourceIndices.size());
    std::fill(old2newIndices.begin(), old2newIndices.end(), -1);

    std::vector<std::vector<int>> vertexToNew;
    vertexToNew.resize(points.size());

    newCounts = counts;
    newIndices.reserve(sourceIndices.size());
    newPoints.reserve(sourceIndices.size());
    new2oldPoints.reserve(sourceIndices.size());

    for (size_t ii = 0; ii < sourceIndices.size(); ++ii) {
        const int vi = sourceIndices[ii];
        if (vi < 0 || vi >= static_cast<int>(points.size())) {
            continue;
        }

        int newIndex = -1;
        auto& candidates = vertexToNew[vi];
        for (int ni : candidates) {
            bool match = true;
            for (const auto& attribute : attributes) {
                if (!attribute->Compare(ni, static_cast<int>(ii))) {
                    match = false;
                    break;
                }
            }
            if (match) {
                newIndex = ni;
                break;
            }
        }

        if (newIndex == -1) {
            newIndex = static_cast<int>(newPoints.size());
            newPoints.push_back(points[vi]);
            new2oldPoints.push_back(vi);
            candidates.push_back(newIndex);
            for (auto& attribute : attributes) {
                attribute->Emit(static_cast<int>(ii));
            }
        }

        old2newIndices[ii] = newIndex;
        newIndices.push_back(newIndex);
    }
}

#pragma region Triangulation
static float Cross2D(const float2& a, const float2& b, const float2& c)
{
    const float2 ab = b - a;
    const float2 ac = c - a;
    return ab[0] * ac[1] - ab[1] * ac[0];
}

static float SignedArea2D(std::span<const float2> points)
{
    float area = 0.0f;
    const size_t n = points.size();
    for (size_t i = 0; i < n; ++i) {
        const float2& a = points[i];
        const float2& b = points[(i + 1) % n];
        area += a[0] * b[1] - b[0] * a[1];
    }
    return area * 0.5f;
}

static bool PointInTriangle2D(const float2& p, const float2& a, const float2& b, const float2& c)
{
    constexpr float kEps = 1e-7f;
    const float c0 = Cross2D(a, b, p);
    const float c1 = Cross2D(b, c, p);
    const float c2 = Cross2D(c, a, p);
    const bool hasNeg = (c0 < -kEps) || (c1 < -kEps) || (c2 < -kEps);
    const bool hasPos = (c0 > kEps) || (c1 > kEps) || (c2 > kEps);
    return !(hasNeg && hasPos);
}

static float3 ComputePolygonNormal(std::span<const float3> points, std::span<const int> indices)
{
    float3 normal(0.0f, 0.0f, 0.0f);
    const int count = static_cast<int>(indices.size());
    for (int i = 0; i < count; ++i) {
        const int i0 = indices[i];
        const int i1 = indices[(i + 1) % count];
        const float3& p0 = points[i0];
        const float3& p1 = points[i1];
        normal[0] += (p0[1] - p1[1]) * (p0[2] + p1[2]);
        normal[1] += (p0[2] - p1[2]) * (p0[0] + p1[0]);
        normal[2] += (p0[0] - p1[0]) * (p0[1] + p1[1]);
    }
    return normal;
}

enum class ProjectionAxis
{
    kZPositive,
    kZNegative,
    kYPositive,
    kYNegative,
    kXPositive,
    kXNegative,
};

static ProjectionAxis SelectProjectionAxis(const float3& normal)
{
    enum class LongestAxis { kX, kY, kZ };
    LongestAxis longest = LongestAxis::kY;
    const float ax = std::abs(normal[0]);
    const float ay = std::abs(normal[1]);
    const float az = std::abs(normal[2]);
    constexpr float kEps = 1e-7f;

    if ((az - ay) > kEps && (az - ax) > kEps) {
        longest = LongestAxis::kZ;
    }
    else if ((ay - ax) > kEps && (ay - az) > kEps) {
        longest = LongestAxis::kY;
    }
    else if ((ax - ay) > kEps && (ax - az) > kEps) {
        longest = LongestAxis::kX;
    }
    else if ((az - ay) > kEps) {
        longest = LongestAxis::kZ;
    }
    else if ((ax - az) > kEps) {
        longest = LongestAxis::kX;
    }

    switch (longest) {
    case LongestAxis::kZ: return normal[2] < 0.0f ? ProjectionAxis::kZNegative : ProjectionAxis::kZPositive;
    case LongestAxis::kX: return normal[0] < 0.0f ? ProjectionAxis::kXNegative : ProjectionAxis::kXPositive;
    case LongestAxis::kY:
    default: return normal[1] < 0.0f ? ProjectionAxis::kYNegative : ProjectionAxis::kYPositive;
    }
}

static float2 ProjectToPolygon2D(const float3& p, ProjectionAxis axis)
{
    switch (axis) {
    case ProjectionAxis::kZPositive: return float2(p[0], p[1]);
    case ProjectionAxis::kZNegative: return float2(p[0], -p[1]);
    case ProjectionAxis::kYPositive: return float2(-p[0], p[2]);
    case ProjectionAxis::kYNegative: return float2(p[0], p[2]);
    case ProjectionAxis::kXPositive: return float2(p[1], p[2]);
    case ProjectionAxis::kXNegative: return float2(-p[1], p[2]);
    default: return float2(0.0f, 0.0f);
    }
}


bool Triangulator::DoEarClipTriangulate()
{
    result.clear();
    const int n = static_cast<int>(polygon2DVertices.size());
    if (n < 3) {
        return false;
    }
    if (n == 3) {
        result.push_back(0);
        result.push_back(1);
        result.push_back(2);
        return true;
    }

    polygon.resize(n);
    std::iota(polygon.begin(), polygon.end(), 0);

    if (SignedArea2D(polygon2DVertices) < 0.0f) {
        std::reverse(polygon.begin(), polygon.end());
    }

    constexpr float kEps = 1e-7f;
    int guard = 0;
    const int maxGuard = n * n;
    while (polygon.size() > 3 && guard < maxGuard) {
        bool earFound = false;
        const int m = static_cast<int>(polygon.size());
        for (int i = 0; i < m; ++i) {
            const int i0 = polygon[(i + m - 1) % m];
            const int i1 = polygon[i];
            const int i2 = polygon[(i + 1) % m];

            const float2& a = polygon2DVertices[i0];
            const float2& b = polygon2DVertices[i1];
            const float2& c = polygon2DVertices[i2];
            if (Cross2D(a, b, c) <= kEps) {
                continue;
            }

            bool containsOther = false;
            for (int j = 0; j < m; ++j) {
                const int ip = polygon[j];
                if (ip == i0 || ip == i1 || ip == i2) {
                    continue;
                }
                if (PointInTriangle2D(polygon2DVertices[ip], a, b, c)) {
                    containsOther = true;
                    break;
                }
            }

            if (containsOther) {
                continue;
            }

            result.push_back(i0);
            result.push_back(i1);
            result.push_back(i2);
            polygon.erase(polygon.begin() + i);
            earFound = true;
            break;
        }

        if (!earFound) {
            return false;
        }
        ++guard;
    }

    if (polygon.size() != 3) {
        return false;
    }

    result.push_back(polygon[0]);
    result.push_back(polygon[1]);
    result.push_back(polygon[2]);
    return true;
}

bool Triangulator::EarClipTriangulate(std::span<const float3> points, std::span<const int> indices)
{
    result.clear();
    if (indices.size() < 3) {
        return false;
    }

    for (int i = 0; i < indices.size(); ++i) {
        const int index = indices[i];
        if (index < 0 || index >= points.size()) {
            return false;
        }
    }

    const float3 normal = ComputePolygonNormal(points, indices);
    const ProjectionAxis axis = SelectProjectionAxis(normal);

    polygon2DVertices.clear();
    polygon2DVertices.reserve(indices.size());
    for (int i = 0; i < indices.size(); ++i) {
        polygon2DVertices.push_back(ProjectToPolygon2D(points[indices[i]], axis));
    }

    if (!DoEarClipTriangulate()) {
        return false;
    }
    return true;
}

bool Triangulator::SimpleTriangulate(std::span<const int> indices)
{
    result.clear();
    if (indices.size() < 3) {
        return false;
    }
    else if (indices.size() == 3) {
        result.push_back(0);
        result.push_back(1);
        result.push_back(2);
        return true;
    }

    int nextVertex = 3;
    int prevVertex = static_cast<int>(indices.size()) - 1;
    int lastVertex = 2;
    int firstVertex = 0;

    const int triangleCount = static_cast<int>(indices.size()) - 2;
    const int totalPoints = triangleCount * 3;
    int written = 0;

    result.push_back(0);
    result.push_back(1);
    result.push_back(2);
    written += 3;

    while (nextVertex <= prevVertex) {
        result.push_back(lastVertex);
        result.push_back(nextVertex);
        result.push_back(firstVertex);
        written += 3;

        if (written == totalPoints) {
            break;
        }

        result.push_back(firstVertex);
        result.push_back(nextVertex);
        result.push_back(prevVertex);
        written += 3;

        lastVertex = nextVertex;
        firstVertex = prevVertex;

        ++nextVertex;
        --prevVertex;
    }
    return true;
}


bool RefineMesh(const MeshData& src, MeshData& dst)
{
    VtArray<int> sourceIndices = src.indices;
    if (sourceIndices.empty()) {
        sourceIndices.resize(src.points.size());
        std::iota(sourceIndices.begin(), sourceIndices.end(), 0);
    }

    size_t totalCount = 0;
    for (int count : src.counts) {
        if (count <= 0) {
            return false;
        }
        totalCount += static_cast<size_t>(count);
    }
    if (totalCount > sourceIndices.size()) {
        return false;
    }

    MeshRefiner refiner;
    refiner.counts = src.counts;
    refiner.indices = sourceIndices;
    refiner.points = src.points;

    VtArray<int> primvarNew2OldNormals;
    VtArray<int> primvarNew2OldBinormals;
    VtArray<int> primvarNew2OldTangents;
    VtArray<int> primvarNew2OldColors;
    VtArray<int> primvarNew2OldUvs[kMaxUvSets];

    auto bindPrimvar = [&]<typename T>(const PrimvarData<T>& srcPrimvar, PrimvarData<T>& dstPrimvar, VtArray<int>& new2old) {
        dstPrimvar.indices.clear();
        dstPrimvar.values.clear();
        new2old.clear();

        if (srcPrimvar.values.empty()) {
            return true;
        }

        if (!srcPrimvar.indices.empty()) {
            if (srcPrimvar.indices.size() < totalCount) {
                return false;
            }
            refiner.AddIndexedAttribute(srcPrimvar.values, srcPrimvar.indices, dstPrimvar.values, new2old);
            return true;
        }

        if (srcPrimvar.values.size() == totalCount) {
            refiner.AddExpandedAttribute(srcPrimvar.values, dstPrimvar.values, new2old);
            return true;
        }

        if (srcPrimvar.values.size() == src.points.size()) {
            refiner.AddIndexedAttribute(srcPrimvar.values, sourceIndices, dstPrimvar.values, new2old);
            return true;
        }

        return false;
    };

    if (!bindPrimvar(src.normals, dst.normals, primvarNew2OldNormals)) return false;
    if (!bindPrimvar(src.binormals, dst.binormals, primvarNew2OldBinormals)) return false;
    if (!bindPrimvar(src.tangents, dst.tangents, primvarNew2OldTangents)) return false;
    if (!bindPrimvar(src.colors, dst.colors, primvarNew2OldColors)) return false;
    for (int i = 0; i < kMaxUvSets; ++i) {
        if (!bindPrimvar(src.uvs[i], dst.uvs[i], primvarNew2OldUvs[i])) return false;
    }

    refiner.Refine();

    dst.counts = refiner.newCounts;
    dst.indices = refiner.newIndices;
    dst.points = refiner.newPoints;
    return true;
}

bool TriangulateMesh(const MeshData& src, MeshData& dst)
{
    size_t totalCount = 0;
    size_t totalTriangles = 0;
    for (int count : src.counts) {
        // count == 1 (points), count == 2 (lines) は無視
        if (count <= 0) {
            return false;
        }
        else if (count >= 3) {
            totalCount += count;
            totalTriangles += count - 2;
        }
    }

    if (totalTriangles == 0 || totalCount > src.indices.size()) {
        return false;
    }
    else if (totalTriangles == src.counts.size() && totalCount == src.indices.size()) {
        // 既に全て三角形
        dst = src;
        return true;
    }

    VtArray<int> baseIndices = src.indices;
    VtArray<float3> basePoints = src.points;
    if (baseIndices.empty()) {
        baseIndices.resize(src.points.size(), [](int* begin, int* end) { std::iota(begin, end, 0); });
    }

    dst.indices.clear();
    dst.indices.reserve(totalTriangles * 3);
    dst.counts.clear();
    dst.counts.resize(totalTriangles, 3);
    dst.points = basePoints;

    auto initializePrimvar = [&]<typename T>(const PrimvarData<T>&srcPV, PrimvarData<T>&dstPV) {
        dstPV.indices.clear();
        dstPV.values.clear();

        if (!srcPV.indices.empty()) {
            dstPV.indices.reserve(totalTriangles * 3);
            dstPV.values = srcPV.values;
        }
        else if (srcPV.values.size() == basePoints.size()) {
            dstPV.values = srcPV.values;
        }
        else if (srcPV.values.size() == baseIndices.size()) {
            dstPV.values.reserve(totalTriangles * 3);
        }
    };
#define Action(name) initializePrimvar(src.name, dst.name);
    EachMeshPrimvar(Action);
#undef Action

    Triangulator ctx;
    size_t offset = 0;
    for (int count : src.counts) {
        std::span<const int> indices{ baseIndices.data() + offset, static_cast<size_t>(count) };

        if (count == 3) {
            for (int i = 0; i < 3; ++i) {
                dst.indices.push_back(indices[i]);
            }

            auto triangulatePrimvar = [&]<typename T>(const PrimvarData<T>&srcPV, PrimvarData<T>&dstPV) {
                if (!srcPV.indices.empty()) {
                    for (int i = 0; i < 3; ++i) {
                        dstPV.indices.push_back(srcPV.indices[offset + i]);
                    }
                }
                else if (srcPV.values.size() == baseIndices.size() && !dstPV.values.IsIdentical(srcPV.values)) {
                    for (int i = 0; i < 3; ++i) {
                        dstPV.values.push_back(srcPV.values[offset + i]);
                    }
                }
            };
#define Action(name) triangulatePrimvar(src.name, dst.name);
            EachMeshPrimvar(Action);
#undef Action
        }
        else if (count > 3 && (ctx.EarClipTriangulate(basePoints, indices) || ctx.SimpleTriangulate(indices))) {
            std::span<const int> triangulated = ctx.GetResult();
            for (int ii : triangulated) {
                dst.indices.push_back(indices[ii]);
            }

            auto triangulatePrimvar = [&]<typename T>(const PrimvarData<T>&srcPV, PrimvarData<T>&dstPV) {
                if (!srcPV.indices.empty()) {
                    for (int ii : triangulated) {
                        dstPV.indices.push_back(srcPV.indices[offset + ii]);
                    }
                }
                else if (srcPV.values.size() == baseIndices.size() && !dstPV.values.IsIdentical(srcPV.values)) {
                    for (int ii : triangulated) {
                        dstPV.values.push_back(srcPV.values[offset + ii]);
                    }
                }
            };
#define Action(name) triangulatePrimvar(src.name, dst.name);
            EachMeshPrimvar(Action);
#undef Action
        }

        offset += count;
    }

    return true;
}
#pragma endregion Triangulation
