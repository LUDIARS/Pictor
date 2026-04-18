// Pictor FBX Demo
//
// Demonstrates the full FBX pipeline:
//   1. FBXImporter::import_file() -> FBXImportResult (Level 1 + 2+3 + 4)
//   2. Register skeleton + clips with AnimationSystem
//   3. Create an animation instance and play the first clip
//   4. Sample bone transforms over time and print them
//
// Usage:
//   pictor_fbx_demo [<path-to-model.fbx>]
//
// With no arguments, iterates over the bundled test set
// `fbx/model1/model.fbx` ... `fbx/model5/model.fbx` and runs the full
// pipeline on every one that exists on disk (missing entries are
// skipped so this works even while only a subset of the models is
// unzipped). Pass an explicit FBX path to fall back to the single-file
// mode.

#include "pictor/animation/fbx_importer.h"
#include "pictor/animation/fbx_scene.h"
#include "pictor/animation/animation_system.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

using namespace pictor;

namespace {

const char* format_name(AnimationFormat f) {
    switch (f) {
        case AnimationFormat::FBX_BINARY: return "FBX binary";
        case AnimationFormat::FBX_ASCII:  return "FBX ASCII";
        default:                          return "unknown";
    }
}

const char* object_type_name(FBXObjectType t) {
    switch (t) {
        case FBXObjectType::GLOBAL_SETTINGS:             return "GlobalSettings";
        case FBXObjectType::MODEL:                       return "Model";
        case FBXObjectType::GEOMETRY:                    return "Geometry";
        case FBXObjectType::MATERIAL:                    return "Material";
        case FBXObjectType::TEXTURE:                     return "Texture";
        case FBXObjectType::VIDEO:                       return "Video";
        case FBXObjectType::DEFORMER_SKIN:               return "Skin";
        case FBXObjectType::DEFORMER_CLUSTER:            return "Cluster";
        case FBXObjectType::DEFORMER_BLENDSHAPE:         return "BlendShape";
        case FBXObjectType::DEFORMER_BLENDSHAPE_CHANNEL: return "BlendShapeChannel";
        case FBXObjectType::SHAPE_GEOMETRY:              return "Shape";
        case FBXObjectType::POSE:                        return "Pose";
        case FBXObjectType::ANIMATION_STACK:             return "AnimStack";
        case FBXObjectType::ANIMATION_LAYER:             return "AnimLayer";
        case FBXObjectType::ANIMATION_CURVE_NODE:        return "AnimCurveNode";
        case FBXObjectType::ANIMATION_CURVE:             return "AnimCurve";
        case FBXObjectType::NODE_ATTRIBUTE:              return "NodeAttribute";
        default:                                         return "Unknown";
    }
}

void print_scene_summary(const FBXImportResult& r) {
    std::printf("== FBX Scene Summary =========================================\n");
    std::printf("  format : %s (v%u)\n", format_name(r.detected_format),
                r.scene ? r.scene->file_version : 0);
    if (r.scene) {
        std::printf("  creator: %s\n", r.scene->creator.c_str());
        std::printf("  app    : %s / %s / %s\n",
                    r.scene->application_name.c_str(),
                    r.scene->application_vendor.c_str(),
                    r.scene->application_version.c_str());
    }

    if (r.scene) {
        const FBXObjectType types[] = {
            FBXObjectType::MODEL, FBXObjectType::GEOMETRY, FBXObjectType::MATERIAL,
            FBXObjectType::TEXTURE, FBXObjectType::VIDEO,
            FBXObjectType::DEFORMER_SKIN, FBXObjectType::DEFORMER_CLUSTER,
            FBXObjectType::DEFORMER_BLENDSHAPE, FBXObjectType::DEFORMER_BLENDSHAPE_CHANNEL,
            FBXObjectType::SHAPE_GEOMETRY, FBXObjectType::POSE,
            FBXObjectType::ANIMATION_STACK, FBXObjectType::ANIMATION_LAYER,
            FBXObjectType::ANIMATION_CURVE_NODE, FBXObjectType::ANIMATION_CURVE,
            FBXObjectType::NODE_ATTRIBUTE,
        };
        std::printf("  objects:\n");
        for (FBXObjectType t : types) {
            auto ids = r.scene->ids_of_type(t);
            if (!ids.empty()) std::printf("    %-18s x %zu\n", object_type_name(t), ids.size());
        }
        std::printf("  root models: %zu\n", r.scene->root_model_ids.size());
    }

    std::printf("  projected descriptors:\n");
    std::printf("    bones          : %zu\n", r.skeleton.bones.size());
    std::printf("    clips          : %zu\n", r.clips.size());
    for (const auto& c : r.clips) {
        std::printf("      - %s : %.3fs, %zu channels\n",
                    c.name.c_str(), c.duration, c.channels.size());
    }
    std::printf("    skin meshes    : %zu\n", r.skin_meshes.size());
    for (const auto& sm : r.skin_meshes) {
        std::printf("      - %s : %u verts, %u indices, %zu morphs\n",
                    sm.name.c_str(), sm.vertex_count, sm.index_count, sm.morph_target_names.size());
    }
    std::printf("    material slots : %zu\n", r.material_slots.size());
    std::printf("    texture paths  : %zu\n", r.texture_paths.size());
    for (const auto& p : r.texture_paths) std::printf("      - %s\n", p.c_str());
    std::printf("==============================================================\n");
}

/// Build a minimal synthetic scene (3 bones, single translation clip) so the
/// demo can be exercised when no FBX file is provided.
void build_synthetic(FBXImportResult& r) {
    r.success         = true;
    r.detected_format = AnimationFormat::UNKNOWN;

    r.skeleton.name = "synthetic";
    r.skeleton.bones.resize(3);
    for (int i = 0; i < 3; ++i) {
        Bone& b = r.skeleton.bones[i];
        b.name = "bone_" + std::to_string(i);
        b.parent_index = i - 1;
        b.bind_pose = Transform::identity();
        b.bind_pose.translation = {(i == 0 ? 0.0f : 1.0f), 0.0f, 0.0f};
        b.inverse_bind_matrix = float4x4::identity();
        b.inverse_bind_matrix.m[3][0] = -(static_cast<float>(i));
    }

    AnimationClipDescriptor clip;
    clip.name = "wave";
    clip.duration = 2.0f;
    clip.sample_rate = 30.0f;
    clip.wrap_mode = WrapMode::LOOP;

    AnimationChannel ch;
    ch.target_index = 2;                           // tip bone
    ch.target       = ChannelTarget::ROTATION;
    ch.interpolation = InterpolationMode::LINEAR;
    for (int k = 0; k <= 4; ++k) {
        float t = static_cast<float>(k) * 0.5f;
        float angle = std::sin(t * 3.1415926f) * 0.8f;
        Quaternion q = Quaternion::from_axis_angle({0, 0, 1}, angle);
        Keyframe kf;
        kf.time = t;
        kf.value[0] = q.x; kf.value[1] = q.y; kf.value[2] = q.z; kf.value[3] = q.w;
        ch.keyframes.push_back(kf);
    }
    clip.channels.push_back(std::move(ch));
    r.clips.push_back(std::move(clip));
}

void print_bone_matrix(const char* label, const float4x4& m) {
    std::printf("    %-20s t=(%+.3f, %+.3f, %+.3f)\n",
                label, m.m[3][0], m.m[3][1], m.m[3][2]);
}

} // namespace

/// Run the full FBX → AnimationSystem pipeline for a single result and
/// print the scene summary plus sampled skinning matrices. Returns true
/// on success (pipeline exercised), false on import failure.
bool run_pipeline(FBXImportResult& result) {
    print_scene_summary(result);

    if (result.skeleton.bones.empty()) {
        std::printf("No bones extracted — nothing to animate.\n");
        return true;
    }

    AnimationSystemConfig cfg;
    cfg.gpu_skinning_enabled   = false;           // CPU path for the demo
    cfg.max_bones_per_skeleton = 512;
    cfg.max_active_instances   = 16;
    AnimationSystem anim;
    anim.initialize(cfg);

    SkeletonHandle sk = anim.register_skeleton(result.skeleton);
    std::vector<AnimationClipHandle> clip_handles;
    for (const auto& c : result.clips) clip_handles.push_back(anim.register_clip(c));

    const ObjectId dummy_object = 42;
    AnimationStateHandle inst = anim.create_instance(dummy_object, sk);
    if (!clip_handles.empty()) {
        anim.play(inst, clip_handles[0], /*weight=*/1.0f, /*speed=*/1.0f);
        std::printf("Playing clip: %s (%.2fs)\n",
                    result.clips[0].name.c_str(), result.clips[0].duration);
    } else {
        std::printf("No clips to play; will print bind pose only.\n");
    }

    const uint32_t bone_count = anim.get_bone_count(inst);
    std::printf("Bones in instance: %u\n\n", bone_count);

    const float dt = 1.0f / 30.0f;               // 30 fps
    const int total_frames = 90;                 // 3 seconds
    const int print_stride = 15;                 // every 0.5s

    for (int frame = 0; frame <= total_frames; ++frame) {
        anim.update(dt);
        if (frame % print_stride != 0) continue;
        float t = frame * dt;
        std::printf("[t=%.2fs frame=%d]\n", t, frame);

        const float4x4* skin = anim.get_skinning_matrices(inst);
        if (skin && bone_count > 0) {
            const uint32_t show = std::min<uint32_t>(bone_count, 4);
            for (uint32_t i = 0; i < show; ++i) {
                std::string label = "bone[" + std::to_string(i) + "] " +
                                     (i < result.skeleton.bones.size() ? result.skeleton.bones[i].name : "");
                print_bone_matrix(label.c_str(), skin[i]);
            }
            if (bone_count > show) {
                std::printf("    ... (%u more)\n", bone_count - show);
            }
        }
    }

    if (result.scene) {
        std::printf("\nResource ID access sample:\n");
        auto mids = result.resource_ids_of_type(FBXObjectType::MODEL);
        for (size_t i = 0; i < std::min<size_t>(mids.size(), 3); ++i) {
            const FBXObject* obj = result.get_resource(mids[i]);
            if (!obj) continue;
            auto ws = result.scene->evaluate_world_transform(mids[i]);
            std::printf("  Model[%llu] '%s' (%s)\n",
                        static_cast<unsigned long long>(mids[i]),
                        obj->name.c_str(),
                        obj->model ? obj->model->sub_type.c_str() : "");
            std::printf("    world_translation=(%+.3f, %+.3f, %+.3f)\n",
                        ws.m[3][0], ws.m[3][1], ws.m[3][2]);
        }
    }

    anim.destroy_instance(inst);
    anim.shutdown();
    return true;
}

/// Import one FBX file and run the pipeline on it. Prints a banner
/// with the source path.
bool run_for_file(const fs::path& path) {
    std::printf("\n================================================================\n");
    std::printf("  Loading: %s\n", path.string().c_str());
    std::printf("================================================================\n");

    FBXImporter importer;
    FBXImportResult result = importer.import_file(path.string());
    if (!result.success) {
        std::printf("FBX import failed: %s\n", result.error_message.c_str());
        return false;
    }
    return run_pipeline(result);
}

int main(int argc, char** argv) {
    std::printf("Pictor FBX Demo\n");
    std::printf("---------------\n");

    if (argc >= 2) {
        return run_for_file(argv[1]) ? 0 : 1;
    }

    // No argument: sweep the bundled test set `fbx/model1` .. `fbx/model5`.
    // Each slot that is present on disk is loaded and exercised; missing
    // ones are skipped (the repo ships only model1 by default, with the
    // rest provided separately — see fbx/README.md).
    int found = 0;
    int failed = 0;
    for (int i = 1; i <= 5; ++i) {
        fs::path p = fs::path("fbx") / ("model" + std::to_string(i)) / "model.fbx";
        if (!fs::exists(p)) {
            std::printf("  [skip] %s (not present)\n", p.string().c_str());
            continue;
        }
        ++found;
        if (!run_for_file(p)) ++failed;
    }

    if (found == 0) {
        std::printf("\nNo FBX models found under fbx/model{1..5}/model.fbx.\n");
        std::printf("Falling back to synthetic scene. Pass a path explicitly\n");
        std::printf("or unzip the test asset bundle into `fbx/` to enable the\n");
        std::printf("full sweep (see fbx/README.md).\n\n");
        FBXImportResult synth;
        build_synthetic(synth);
        run_pipeline(synth);
    } else {
        std::printf("\n--- Sweep summary: %d FBX processed, %d failed ---\n",
                    found, failed);
    }

    std::printf("\nDone.\n");
    return failed == 0 ? 0 : 1;
}
