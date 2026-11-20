#include "../istTerrain/pch.h"
#include "../istTerrain/meshUtils.h"

#include <chrono>
#include <cmath>
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

static const GfMatrix4f kProjective(
     2.0f,  1.0f, -3.0f, 1.0f,
    -1.0f,  3.0f,  2.0f, 0.0f,
     4.0f, -2.0f,  0.5f, 0.0f,
     5.0f,  7.0f, -9.0f, 0.0f);

static bool VerifyTransforms(std::span<GfVec3f> points, const GfMatrix4f& transform)
{
    const std::vector<GfVec3f> original(points.begin(), points.end());
    std::vector<GfVec3f> output(points.size());
    const auto verify = [&](std::span<const GfVec3f> expectedInput, std::span<const GfVec3f> actual,
                            bool isPoint, const char* mode) {
        for (size_t i = 0; i < actual.size(); ++i) {
            const GfVec3f expected = isPoint ? transform.Transform(expectedInput[i]) : transform.TransformDir(expectedInput[i]);
            for (size_t axis = 0; axis < 3; ++axis) {
                if (actual[i][axis] != expected[axis] &&
                    !(std::isnan(actual[i][axis]) && std::isnan(expected[axis]))) {
                    std::cerr << (isPoint ? "Point" : "Vector") << " transform mismatch (" << mode << ") at vertex "
                              << i << ", axis " << axis << " for " << actual.size() << " vertices\n";
                    return false;
                }
            }
        }
        return true;
    };

    for (bool isPoint : {true, false}) {
        if (isPoint)
            FastTransformPoints(output, original, transform);
        else
            FastTransformVectors(output, original, transform);
        if (!verify(original, output, isPoint, "separate buffers")) return false;

        std::copy(original.begin(), original.end(), points.begin());
        const std::span<const GfVec3f> input(points.data(), points.size());
        if (isPoint)
            FastTransformPoints(points, input, transform);
        else
            FastTransformVectors(points, input, transform);
        if (!verify(original, points, isPoint, "aliased buffers")) return false;
    }
    std::copy(original.begin(), original.end(), points.begin());
    return true;
}

static bool TestTransforms()
{
    GfMatrix4f affine = kProjective;
    affine[0][3] = 0.0f;
    affine[3][3] = 1.0f;
    GfVec3f points[25];
    for (int i = 0; i < 25; ++i)
        points[i] = GfVec3f(float(i % 7 - 3), float(i - 8), float(i % 5 - 2));
    points[2][0] = std::numeric_limits<float>::infinity();
    points[10][1] = std::numeric_limits<float>::quiet_NaN();
    for (const GfMatrix4f& transform : {GfMatrix4f(1.0f), affine, kProjective}) {
        for (size_t count = 0; count <= 24; ++count) {
            if (!VerifyTransforms({points + 1, count}, transform)) return false;
        }
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
        ok = Verify({points, count}) && VerifyTransforms({points, count}, kProjective);
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
    if (!TestTransforms() || !Verify({}) || !TestPageBoundary()) return 1;
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
    std::cout << "FastBounds and FastTransform tests passed\nReference: " << referenceMs
              << " ms, FastBounds: " << fastMs << " ms\n";
    return 0;
}
