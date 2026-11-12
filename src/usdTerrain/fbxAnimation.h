#pragma once
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/gf/quatf.h>
#include <pxr/base/vt/array.h>
#include <pxr/usd/sdf/path.h>
#include <pxr/base/tf/token.h>
#include <vector>
#include <string_view>


struct AnimationData
{
    struct TransformData
    {
        VtArray<double> timeSamples;
        VtArray<GfVec3f> translation;
        VtArray<GfQuatf> rotation;
        VtArray<GfVec3f> scale;
    };

    struct BlendShapeData
    {
        TfToken blendShapeName;
        VtArray<double> timeSamples;
        VtArray<float> weights;
    };


    SdfPath path;
    TransformData transform;
    std::vector<BlendShapeData> blendShape;
};

bool LoadFbxAnimation(std::string_view fbxFilePath, std::vector<AnimationData>& outAnimations);
