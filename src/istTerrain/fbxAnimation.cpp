#include "pch.h"
#include "fbxAnimation.h"
#include <ufbx/ufbx.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>

#include <pxr/base/tf/stringUtils.h>


static std::string ToStdString(ufbx_string str)
{
    return std::string(str.data, str.length);
}

static bool UfbxStringEquals(ufbx_string lhs, const char* rhs)
{
    const size_t rhsLen = std::strlen(rhs);
    return lhs.length == rhsLen && std::memcmp(lhs.data, rhs, rhsLen) == 0;
}

static GfVec3f ToGfVec3f(ufbx_vec3 value)
{
    return GfVec3f(
        static_cast<float>(value.x),
        static_cast<float>(value.y),
        static_cast<float>(value.z));
}

static GfQuatf ToGfQuatf(ufbx_quat value)
{
    return GfQuatf(
        static_cast<float>(value.w),
        GfVec3f(
            static_cast<float>(value.x),
            static_cast<float>(value.y),
            static_cast<float>(value.z)));
}

static SdfPath BuildNodePath(const ufbx_node* node)
{
    std::vector<const ufbx_node*> hierarchy;
    for (const ufbx_node* current = node; current && current->parent; current = current->parent) {
        hierarchy.push_back(current);
    }
    std::reverse(hierarchy.begin(), hierarchy.end());

    SdfPath path = SdfPath::AbsoluteRootPath();
    for (const ufbx_node* current : hierarchy) {
        std::string name = TfMakeValidIdentifier(ToStdString(current->name));
        if (name.empty()) {
            name = "Node";
        }

        name += "_" + std::to_string(current->typed_id);
        path = path.AppendChild(TfToken(name));
    }

    if (path == SdfPath::AbsoluteRootPath()) {
        path = path.AppendChild(TfToken("Node_0"));
    }
    return path;
}

static void AppendTimes(ufbx_baked_vec3_list keys, std::vector<double>& dst)
{
    for (size_t i = 0; i < keys.count; ++i) {
        dst.push_back(keys.data[i].time);
    }
}

static void AppendTimes(ufbx_baked_quat_list keys, std::vector<double>& dst)
{
    for (size_t i = 0; i < keys.count; ++i) {
        dst.push_back(keys.data[i].time);
    }
}

static AnimationData& GetOrAddAnimation(
    std::unordered_map<std::string, size_t>& pathToIndex,
    std::vector<AnimationData>& animations,
    const SdfPath& path)
{
    const std::string pathString = path.GetString();
    if (auto it = pathToIndex.find(pathString); it != pathToIndex.end()) {
        return animations[it->second];
    }

    const size_t index = animations.size();
    pathToIndex.emplace(pathString, index);
    animations.push_back({});
    animations.back().path = path;
    return animations.back();
}

bool LoadFbxAnimation(std::string_view fbxFilePath, std::vector<AnimationData>& outAnimations)
{
    outAnimations.clear();

    ufbx_error error{};
    std::unique_ptr<ufbx_scene, decltype(&ufbx_free_scene)> scene(
        ufbx_load_file_len(fbxFilePath.data(), fbxFilePath.length(), nullptr, &error),
        &ufbx_free_scene);
    if (!scene) {
        return false;
    }

    const ufbx_anim* anim = scene->anim;
    if (scene->anim_stacks.count > 0 && scene->anim_stacks.data[0] && scene->anim_stacks.data[0]->anim) {
        anim = scene->anim_stacks.data[0]->anim;
    }

    if (!anim) {
        return true;
    }

    ufbx_bake_opts bakeOpts{};
    bakeOpts.resample_rate = scene->settings.frames_per_second > 0.0
        ? scene->settings.frames_per_second
        : 30.0;

    std::unique_ptr<ufbx_baked_anim, decltype(&ufbx_free_baked_anim)> baked(
        ufbx_bake_anim(scene.get(), anim, &bakeOpts, &error),
        &ufbx_free_baked_anim);
    if (!baked) {
        return false;
    }

    std::unordered_map<std::string, size_t> pathToIndex;
    std::unordered_map<uint32_t, SdfPath> nodePathByTypedId;
    nodePathByTypedId.reserve(scene->nodes.count);
    for (size_t i = 0; i < scene->nodes.count; ++i) {
        const ufbx_node* node = scene->nodes.data[i];
        nodePathByTypedId.emplace(node->typed_id, BuildNodePath(node));
    }

    for (size_t i = 0; i < baked->nodes.count; ++i) {
        const ufbx_baked_node& bakedNode = baked->nodes.data[i];

        const ufbx_node* sourceNode = nullptr;
        if (bakedNode.typed_id < scene->nodes.count) {
            sourceNode = scene->nodes.data[bakedNode.typed_id];
        }

        if (!sourceNode) {
            continue;
        }

        auto pathIt = nodePathByTypedId.find(bakedNode.typed_id);
        if (pathIt == nodePathByTypedId.end()) {
            continue;
        }

        std::vector<double> times;
        AppendTimes(bakedNode.translation_keys, times);
        AppendTimes(bakedNode.rotation_keys, times);
        AppendTimes(bakedNode.scale_keys, times);

        if (times.empty()) {
            continue;
        }

        std::sort(times.begin(), times.end());
        times.erase(std::unique(times.begin(), times.end()), times.end());

        AnimationData& animation = GetOrAddAnimation(pathToIndex, outAnimations, pathIt->second);
        animation.transform.timeSamples.resize(times.size());
        animation.transform.translation.resize(times.size());
        animation.transform.rotation.resize(times.size());
        animation.transform.scale.resize(times.size());

        for (size_t keyIndex = 0; keyIndex < times.size(); ++keyIndex) {
            const double time = times[keyIndex];
            animation.transform.timeSamples[keyIndex] = time;

            const ufbx_vec3 translation = bakedNode.translation_keys.count > 0
                ? ufbx_evaluate_baked_vec3(bakedNode.translation_keys, time)
                : sourceNode->local_transform.translation;
            animation.transform.translation[keyIndex] = ToGfVec3f(translation);

            const ufbx_quat rotation = bakedNode.rotation_keys.count > 0
                ? ufbx_evaluate_baked_quat(bakedNode.rotation_keys, time)
                : sourceNode->local_transform.rotation;
            animation.transform.rotation[keyIndex] = ToGfQuatf(rotation);

            const ufbx_vec3 scale = bakedNode.scale_keys.count > 0
                ? ufbx_evaluate_baked_vec3(bakedNode.scale_keys, time)
                : sourceNode->local_transform.scale;
            animation.transform.scale[keyIndex] = ToGfVec3f(scale);
        }
    }

    for (size_t meshIndex = 0; meshIndex < scene->meshes.count; ++meshIndex) {
        const ufbx_mesh* mesh = scene->meshes.data[meshIndex];

        for (size_t instanceIndex = 0; instanceIndex < mesh->instances.count; ++instanceIndex) {
            const ufbx_node* instanceNode = mesh->instances.data[instanceIndex];
            if (!instanceNode) {
                continue;
            }

            auto pathIt = nodePathByTypedId.find(instanceNode->typed_id);
            if (pathIt == nodePathByTypedId.end()) {
                continue;
            }

            AnimationData& animation = GetOrAddAnimation(pathToIndex, outAnimations, pathIt->second);

            for (size_t deformerIndex = 0; deformerIndex < mesh->blend_deformers.count; ++deformerIndex) {
                const ufbx_blend_deformer* blendDeformer = mesh->blend_deformers.data[deformerIndex];

                for (size_t channelIndex = 0; channelIndex < blendDeformer->channels.count; ++channelIndex) {
                    const ufbx_blend_channel* channel = blendDeformer->channels.data[channelIndex];
                    ufbx_baked_element* bakedElement = ufbx_find_baked_element_by_element_id(
                        baked.get(),
                        channel->element_id);
                    if (!bakedElement) {
                        continue;
                    }

                    const ufbx_baked_prop* deformPercent = nullptr;
                    for (size_t propIndex = 0; propIndex < bakedElement->props.count; ++propIndex) {
                        const ufbx_baked_prop& prop = bakedElement->props.data[propIndex];
                        if (UfbxStringEquals(prop.name, UFBX_DeformPercent)) {
                            deformPercent = &prop;
                            break;
                        }
                    }

                    if (!deformPercent || deformPercent->keys.count == 0) {
                        continue;
                    }

                    AnimationData::BlendShapeData blendShapeData;
                    blendShapeData.blendShapeName = TfToken(ToStdString(channel->name));
                    blendShapeData.timeSamples.resize(deformPercent->keys.count);
                    blendShapeData.weights.resize(deformPercent->keys.count);

                    for (size_t keyIndex = 0; keyIndex < deformPercent->keys.count; ++keyIndex) {
                        const double time = deformPercent->keys.data[keyIndex].time;
                        blendShapeData.timeSamples[keyIndex] = time;
                        blendShapeData.weights[keyIndex] = static_cast<float>(
                            ufbx_evaluate_blend_weight(anim, channel, time));
                    }

                    animation.blendShape.push_back(std::move(blendShapeData));
                }
            }
        }
    }

    return true;
}
