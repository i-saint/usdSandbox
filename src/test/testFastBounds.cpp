#include "../istTerrain/pch.h"
#include "../istTerrain/meshUtils.h"

#include <chrono>
#include <iostream>
#include <limits>
#include <new>
#include <random>

__declspec(noinline) static GfRange3f ReferenceBounds(std::span<const GfVec3f> points)
{
    GfRange3f bounds;
    for (const GfVec3f& point : points)
        bounds.UnionWith(point);
    return bounds;
}

static bool Verify(std::span<const GfVec3f> points)
{
    const GfRange3f expected = ReferenceBounds(points);
    const GfRange3f actual = FastBounds(points);
    if (expected.GetMin() != actual.GetMin() || expected.GetMax() != actual.GetMax()) {
        std::cerr << "Bounds mismatch for " << points.size() << " points\n";
        return false;
    }
    return true;
}

static bool TestPageBoundary()
{
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    const size_t pageSize = info.dwPageSize;
    void* memory = VirtualAlloc(nullptr, pageSize * 2, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!memory) { std::cerr << "VirtualAlloc failed\n"; return false; }
    auto* boundary = static_cast<unsigned char*>(memory) + pageSize;
    DWORD previous;
    if (!VirtualProtect(boundary, pageSize, PAGE_NOACCESS, &previous)) {
        std::cerr << "VirtualProtect failed\n";
        VirtualFree(memory, 0, MEM_RELEASE);
        return false;
    }
    bool ok = true;
    for (size_t count = 0; count <= 64 && ok; ++count) {
        auto* points = reinterpret_cast<GfVec3f*>(boundary - count * sizeof(GfVec3f));
        for (size_t i = 0; i < count; ++i)
            new (points + i) GfVec3f(float(i), -float(i), float(i % 7));
        ok = Verify({points, count});
        for (size_t i = 0; i < count; ++i)
            points[i].~GfVec3f();
    }
    if (!VirtualFree(memory, 0, MEM_RELEASE)) {
        std::cerr << "VirtualFree failed\n";
        return false;
    }
    return ok;
}

int main()
{
    if (!Verify({}) || !TestPageBoundary()) return 1;
    std::mt19937 random(0x4132);
    std::uniform_real_distribution<float> distribution(-10000.0f, 10000.0f);
    std::vector<GfVec3f> points(1024 * 1024);
    for (GfVec3f& point : points)
        point = GfVec3f(distribution(random), distribution(random), distribution(random));
    for (size_t offset = 0; offset < 8; ++offset) {
        for (size_t count = 0; count <= 257; ++count) {
            if (!Verify(std::span<const GfVec3f>(points.data() + offset, count))) return 1;
        }
    }
    if (!Verify(points)) return 1;
    for (size_t axis = 0; axis < 3; ++axis) {
        for (size_t lane = 0; lane < 24; ++lane) {
            std::vector<GfVec3f> extrema(33, GfVec3f(2.0f));
            extrema[lane][axis] = -100.0f;
            extrema[32 - lane][axis] = 100.0f;
            if (!Verify(extrema)) return 1;
        }
    }
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float infinity = std::numeric_limits<float>::infinity();
    for (const GfVec3f& value : {GfVec3f(5.0f), GfVec3f(-5.0f), GfVec3f(0.0f), GfVec3f(nan), GfVec3f(infinity), GfVec3f(-infinity)}) {
        for (size_t count : {size_t(1), size_t(7), size_t(8), size_t(9), size_t(32)}) {
            std::vector<GfVec3f> special(count, value);
            if (!Verify(special)) return 1;
        }
    }
    std::vector<GfVec3f> mixed(37, GfVec3f(1, 2, 3));
    for (size_t i = 0; i < mixed.size(); ++i) {
        mixed[i][i % 3] = nan;
        if (!Verify(mixed)) return 1;
    }
    using BoundsFunction = GfRange3f(*)(std::span<const GfVec3f>);
    volatile float sink = 0;
    auto benchmark = [&](BoundsFunction function) {
        const auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < 32; ++i)
            sink = sink + function(points).GetMin()[0];
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    };
    const double referenceMs = benchmark(ReferenceBounds);
    const double fastMs = benchmark(FastBounds);
    std::cout << "FastBounds tests passed\nReference: " << referenceMs << " ms, FastBounds: " << fastMs << " ms\n";
    return 0;
}
