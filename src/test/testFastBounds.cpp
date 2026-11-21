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
            TransformPoints(output, original, transform);
        else
            TransformVectors(output, original, transform);
        if (!verify(original, output, isPoint, "separate buffers")) return false;

        std::copy(original.begin(), original.end(), points.begin());
        const std::span<const GfVec3f> input(points.data(), points.size());
        if (isPoint)
            TransformPoints(points, input, transform);
        else
            TransformVectors(points, input, transform);
        if (!verify(original, points, isPoint, "aliased buffers")) return false;
    }
    std::copy(original.begin(), original.end(), points.begin());
    return true;
}

static bool TestNormalizeVectors()
{
    std::vector<GfVec3f> original = {
        GfVec3f(0.0f), GfVec3f(1e-8f, -2e-8f, 3e-8f), GfVec3f(1.0f, 2.0f, 3.0f),
        GfVec3f(-4.0f, 5.0f, -6.0f), GfVec3f(7.0f, 0.0f, 0.0f), GfVec3f(0.0f, -8.0f, 0.0f),
        GfVec3f(0.0f, 0.0f, 9.0f), GfVec3f(-1.0f, -2.0f, -3.0f), GfVec3f(12.0f, 5.0f, 0.0f),
        GfVec3f(0.25f, -0.5f, 1.0f), GfVec3f(3.0f, 4.0f, 12.0f)
    };
    std::vector<GfVec3f> output(original.size());
    for (size_t count = 0; count <= original.size(); ++count) {
        NormalizeVectors(std::span<GfVec3f>(output.data(), count), std::span<const GfVec3f>(original.data(), count));
        for (size_t i = 0; i < count; ++i) {
            const GfVec3f expected = original[i].GetNormalized();
            for (size_t axis = 0; axis < 3; ++axis) {
                if (std::abs(output[i][axis] - expected[axis]) > 1e-6f) {
                    std::cerr << "Normalize mismatch at vertex " << i << ", axis " << axis << "\n";
                    return false;
                }
            }
        }
    }

    std::vector<GfVec3f> inPlace = original;
    NormalizeVectors(inPlace, std::span<const GfVec3f>(inPlace.data(), inPlace.size()));
    for (size_t i = 0; i < inPlace.size(); ++i) {
        const GfVec3f expected = original[i].GetNormalized();
        for (size_t axis = 0; axis < 3; ++axis) {
            if (std::abs(inPlace[i][axis] - expected[axis]) > 1e-6f) {
                std::cerr << "In-place normalize mismatch at vertex " << i << ", axis " << axis << "\n";
                return false;
            }
        }
    }
    return true;
}

template<class T, int Components>
static float LerpComponent(const T& value, size_t component)
{
    if constexpr (std::is_same_v<T, float>)
        return value;
    else
        return value[component];
}

template<class T, int Components>
static bool TestLerpType()
{
    constexpr size_t testSize = 19;
    constexpr float weight = 0.37f;
    std::vector<T> a(testSize), b(testSize), output(testSize);
    for (size_t i = 0; i < testSize; ++i) {
        for (int component = 0; component < Components; ++component) {
            const float av = static_cast<float>(i * 3 + component) - 12.0f;
            const float bv = 20.0f - static_cast<float>(i * 2 + component * 4);
            if constexpr (std::is_same_v<T, float>) {
                a[i] = av;
                b[i] = bv;
            }
            else {
                a[i][component] = av;
                b[i][component] = bv;
            }
        }
    }

    const auto verify = [&](std::span<const T> srcA, std::span<const T> srcB,
                            std::span<const T> actual, const char* mode) {
        for (size_t i = 0; i < actual.size(); ++i) {
            for (int component = 0; component < Components; ++component) {
                const float expected = LerpComponent<T, Components>(srcA[i], component) * (1.0f - weight) +
                    LerpComponent<T, Components>(srcB[i], component) * weight;
                if (std::abs(LerpComponent<T, Components>(actual[i], component) - expected) > 1e-5f) {
                    std::cerr << "Lerp mismatch (" << mode << ") at item " << i << ", component " << component << "\n";
                    return false;
                }
            }
        }
        return true;
    };

    for (size_t count = 0; count <= testSize; ++count) {
        const std::span<const T> srcA(a.data(), count), srcB(b.data(), count);
        std::span<T> dst(output.data(), count);
        Lerp(dst, srcA, srcB, weight);
        if (!verify(srcA, srcB, dst, "separate buffers")) return false;

        std::vector<T> aliasA(a.begin(), a.begin() + count);
        Lerp(std::span<T>(aliasA), std::span<const T>(aliasA), srcB, weight);
        if (!verify(srcA, srcB, aliasA, "first input alias")) return false;

        std::vector<T> aliasB(b.begin(), b.begin() + count);
        Lerp(std::span<T>(aliasB), srcA, std::span<const T>(aliasB), weight);
        if (!verify(srcA, srcB, aliasB, "second input alias")) return false;
    }
    return true;
}

static bool TestLerp()
{
    return TestLerpType<float, 1>() && TestLerpType<GfVec2f, 2>() && TestLerpType<GfVec3f, 3>();
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

static bool TestTangents()
{
    constexpr size_t triangleCount = 10;
    std::vector<GfVec3f> vertices;
    std::vector<GfVec2f> uvs;
    std::vector<GfVec3f> normals;
    std::vector<int> indices;
    vertices.emplace_back(0.0f, 0.0f, 0.0f);
    uvs.emplace_back(0.0f, 0.0f);
    normals.emplace_back(0.0f, 0.0f, 1.0f);
    constexpr float tau = 6.28318530718f;
    for (size_t i = 0; i < triangleCount; ++i) {
        const float angle = tau * static_cast<float>(i) / static_cast<float>(triangleCount);
        const float x = std::cos(angle), y = std::sin(angle);
        vertices.emplace_back(x, y, 0.0f);
        uvs.emplace_back(x, y);
        normals.emplace_back(0.0f, 0.0f, 1.0f);
    }
    for (size_t triangle = 0; triangle < triangleCount; ++triangle) {
        indices.insert(indices.end(), {0, static_cast<int>(triangle + 1),
            static_cast<int>((triangle + 1) % triangleCount + 1)});
    }
    vertices.emplace_back(100.0f, 100.0f, 0.0f);
    uvs.emplace_back(0.0f, 0.0f);
    normals.emplace_back(0.0f, 0.0f, 1.0f);

    std::vector<GfVec4f> tangents4(vertices.size());
    std::vector<GfVec3f> tangents3(vertices.size());
    GenerateTangentsTriangleIndexed(tangents4, vertices, uvs, normals, indices);
    GenerateTangentsTriangleIndexed(tangents3, vertices, uvs, normals, indices);
    for (size_t i = 0; i <= triangleCount; ++i) {
        if (std::abs(tangents4[i][0] - 1.0f) > 1e-5f || std::abs(tangents4[i][1]) > 1e-5f ||
            std::abs(tangents4[i][2]) > 1e-5f || tangents4[i][3] != 1.0f ||
            tangents3[i] != GfVec3f(tangents4[i][0], tangents4[i][1], tangents4[i][2])) {
            std::cerr << "Indexed tangent mismatch at vertex " << i << "\n";
            return false;
        }
    }
    if (std::abs(tangents4.back()[0] - 1.0f) > 1e-5f || tangents4.back()[3] != 1.0f) {
        std::cerr << "Unused vertex tangent fallback mismatch\n";
        return false;
    }

    std::vector<GfVec3f> triangleVertices = {GfVec3f(0, 0, 0), GfVec3f(1, 0, 0), GfVec3f(0, 1, 0)};
    std::vector<GfVec3f> triangleNormals(3, GfVec3f(0, 0, 1));
    const int triangleIndices[] = {0, 1, 2};
    std::vector<GfVec2f> mirroredUvs = {GfVec2f(0, 0), GfVec2f(1, 0), GfVec2f(0, -1)};
    GfVec4f mirrored[3];
    GenerateTangentsTriangleIndexed(mirrored, triangleVertices, mirroredUvs, triangleNormals, triangleIndices);
    for (const GfVec4f& tangent : mirrored) {
        if (std::abs(tangent[0] - 1.0f) > 1e-5f || tangent[3] != -1.0f) {
            std::cerr << "Mirrored-UV tangent handedness mismatch\n";
            return false;
        }
    }

    std::vector<GfVec2f> degenerateUvs(3, GfVec2f(0.0f));
    GfVec4f fallback[3];
    GenerateTangentsTriangleIndexed(fallback, triangleVertices, degenerateUvs, triangleNormals, triangleIndices);
    for (const GfVec4f& tangent : fallback) {
        if (std::abs(tangent[0] - 1.0f) > 1e-5f || std::abs(tangent[1]) > 1e-5f ||
            std::abs(tangent[2]) > 1e-5f || tangent[3] != 1.0f) {
            std::cerr << "Degenerate-UV tangent fallback mismatch\n";
            return false;
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
    if (!TestNormalizeVectors() || !TestLerp() || !TestTransforms() || !TestTangents() || !Verify({}) || !TestPageBoundary()) return 1;
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
    std::cout << "FastBounds, FastTransform, and tangent tests passed\nReference: " << referenceMs
              << " ms, FastBounds: " << fastMs << " ms\n";
    return 0;
}
