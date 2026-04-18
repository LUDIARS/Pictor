#include "pictor/animation/animation_system.h"
#include <algorithm>

namespace pictor {

AnimationSystem::AnimationSystem(const AnimationSystemConfig& config) {
    initialize(config);
}

AnimationSystem::~AnimationSystem() {
    if (initialized_) shutdown();
}

void AnimationSystem::initialize(const AnimationSystemConfig& config) {
    config_ = config;
    initialized_ = true;
}

void AnimationSystem::shutdown() {
    instances_.clear();
    clips_.clear();
    skeletons_.clear();
    anim_2d_players_.clear();
    rive_instances_.clear();
    lottie_instances_.clear();
    vector_players_.clear();
    active_instance_count_ = 0;
    initialized_ = false;
}

// ============================================================
// Resource Registration
// ============================================================

SkeletonHandle AnimationSystem::register_skeleton(const SkeletonDescriptor& desc) {
    SkeletonHandle handle = next_skeleton_handle_++;
    skeletons_[handle] = std::make_unique<Skeleton>(desc);
    return handle;
}

void AnimationSystem::unregister_skeleton(SkeletonHandle handle) {
    skeletons_.erase(handle);
}

AnimationClipHandle AnimationSystem::register_clip(const AnimationClipDescriptor& desc) {
    AnimationClipHandle handle = next_clip_handle_++;
    clips_[handle] = std::make_unique<AnimationClip>(desc);
    return handle;
}

void AnimationSystem::unregister_clip(AnimationClipHandle handle) {
    clips_.erase(handle);
}

// ============================================================
// Animation Instance Management
// ============================================================

AnimationStateHandle AnimationSystem::create_instance(ObjectId object_id,
                                                       SkeletonHandle skeleton) {
    AnimationStateHandle handle = next_instance_handle_++;

    auto instance = std::make_unique<AnimationInstance>();
    instance->object_id = object_id;
    instance->skeleton = skeleton;

    // Allocate pose buffers if skeleton is valid
    auto skel_it = skeletons_.find(skeleton);
    if (skel_it != skeletons_.end()) {
        uint32_t bone_count = skel_it->second->bone_count();
        instance->local_pose.resize(bone_count);
        instance->world_matrices.resize(bone_count);
        instance->skinning_matrices.resize(bone_count);

        // Initialize with bind pose
        skel_it->second->get_bind_pose(instance->local_pose.data());
    }

    instances_[handle] = std::move(instance);
    ++active_instance_count_;
    return handle;
}

void AnimationSystem::destroy_instance(AnimationStateHandle handle) {
    auto it = instances_.find(handle);
    if (it != instances_.end()) {
        instances_.erase(it);
        --active_instance_count_;
    }
}

void AnimationSystem::play(AnimationStateHandle handle, AnimationClipHandle clip,
                            float weight, float speed, AnimBlendMode blend) {
    auto it = instances_.find(handle);
    if (it == instances_.end()) return;

    AnimationPlayback playback;
    playback.clip = clip;
    playback.weight = weight;
    playback.speed = speed;
    playback.blend = blend;
    playback.playing = true;
    playback.time = 0.0f;

    it->second->layers.push_back(playback);
}

void AnimationSystem::stop(AnimationStateHandle handle, AnimationClipHandle clip) {
    auto it = instances_.find(handle);
    if (it == instances_.end()) return;

    auto& layers = it->second->layers;
    layers.erase(
        std::remove_if(layers.begin(), layers.end(),
                       [clip](const AnimationPlayback& p) { return p.clip == clip; }),
        layers.end());
}

void AnimationSystem::stop_all(AnimationStateHandle handle) {
    auto it = instances_.find(handle);
    if (it == instances_.end()) return;
    it->second->layers.clear();
}

void AnimationSystem::set_layer_weight(AnimationStateHandle handle, uint32_t layer, float weight) {
    auto it = instances_.find(handle);
    if (it == instances_.end() || layer >= it->second->layers.size()) return;
    it->second->layers[layer].weight = weight;
}

void AnimationSystem::set_layer_speed(AnimationStateHandle handle, uint32_t layer, float speed) {
    auto it = instances_.find(handle);
    if (it == instances_.end() || layer >= it->second->layers.size()) return;
    it->second->layers[layer].speed = speed;
}

// ============================================================
// IK / FK
// ============================================================

void AnimationSystem::add_ik_chain(AnimationStateHandle handle,
                                    const IKChainDescriptor& chain) {
    auto it = instances_.find(handle);
    if (it == instances_.end()) return;
    it->second->ik_chains.push_back(chain);
}

void AnimationSystem::clear_ik_chains(AnimationStateHandle handle) {
    auto it = instances_.find(handle);
    if (it == instances_.end()) return;
    it->second->ik_chains.clear();
}

void AnimationSystem::set_ik_target(AnimationStateHandle handle, uint32_t chain_index,
                                     const float3& position, const Quaternion& rotation) {
    auto it = instances_.find(handle);
    if (it == instances_.end() || chain_index >= it->second->ik_chains.size()) return;
    it->second->ik_chains[chain_index].target_position = position;
    it->second->ik_chains[chain_index].target_rotation = rotation;
}

void AnimationSystem::set_fk_override(AnimationStateHandle handle, uint32_t bone_index,
                                       const Transform& local_transform, float blend_weight) {
    auto it = instances_.find(handle);
    if (it == instances_.end()) return;

    // Check if override already exists for this bone
    for (auto& fk : it->second->fk_overrides) {
        if (fk.bone_index == bone_index) {
            fk.local_transform = local_transform;
            fk.blend_weight = blend_weight;
            return;
        }
    }

    FKPoseOverride override;
    override.bone_index = bone_index;
    override.local_transform = local_transform;
    override.blend_weight = blend_weight;
    it->second->fk_overrides.push_back(override);
}

void AnimationSystem::clear_fk_overrides(AnimationStateHandle handle) {
    auto it = instances_.find(handle);
    if (it == instances_.end()) return;
    it->second->fk_overrides.clear();
}

// ============================================================
// Motion Estimation
// ============================================================

MotionDistanceResult AnimationSystem::estimate_motion(AnimationClipHandle clip,
                                                       SkeletonHandle skeleton,
                                                       float sample_rate) const {
    auto clip_it = clips_.find(clip);
    auto skel_it = skeletons_.find(skeleton);
    if (clip_it == clips_.end() || skel_it == skeletons_.end()) return {};

    return motion_estimator_.estimate(*clip_it->second, *skel_it->second, sample_rate);
}

// ============================================================
// Update
// ============================================================

void AnimationSystem::update(float delta_time) {
    if (!initialized_) return;

    for (auto& [handle, instance] : instances_) {
        evaluate_instance(*instance, delta_time);
    }
}

void AnimationSystem::evaluate_instance(AnimationInstance& instance, float delta_time) {
    auto skel_it = skeletons_.find(instance.skeleton);
    if (skel_it == skeletons_.end()) return;

    const Skeleton& skeleton = *skel_it->second;
    uint32_t bone_count = skeleton.bone_count();
    if (bone_count == 0) return;

    // Start with bind pose
    skeleton.get_bind_pose(instance.local_pose.data());

    // Evaluate and blend animation layers
    blend_layers(instance);

    // Apply FK overrides
    if (!instance.fk_overrides.empty()) {
        IKSolver::apply_fk_overrides(instance.fk_overrides, instance.local_pose.data());
    }

    // Compute world matrices (needed for IK)
    skeleton.compute_world_matrices(instance.local_pose.data(),
                                    instance.world_matrices.data());

    // Apply IK chains
    apply_ik(instance);

    // Compute final skinning matrices
    skeleton.compute_skinning_matrices(instance.local_pose.data(),
                                       instance.skinning_matrices.data());

    // Advance playback time for all layers
    for (auto& layer : instance.layers) {
        if (layer.playing) {
            layer.time += delta_time * layer.speed;
        }
    }

    // Remove finished non-looping animations
    instance.layers.erase(
        std::remove_if(instance.layers.begin(), instance.layers.end(),
                       [this](const AnimationPlayback& p) {
                           if (!p.playing) return false;
                           auto it = clips_.find(p.clip);
                           if (it == clips_.end()) return true;
                           return p.wrap == WrapMode::ONCE && p.time >= it->second->duration();
                       }),
        instance.layers.end());
}

void AnimationSystem::blend_layers(AnimationInstance& instance) {
    if (instance.layers.empty()) return;

    auto skel_it = skeletons_.find(instance.skeleton);
    if (skel_it == skeletons_.end()) return;

    uint32_t bone_count = skel_it->second->bone_count();
    std::vector<Transform> temp_pose(bone_count);

    for (const auto& layer : instance.layers) {
        auto clip_it = clips_.find(layer.clip);
        if (clip_it == clips_.end()) continue;

        const AnimationClip& clip = *clip_it->second;

        // Initialize temp pose with bind pose
        skel_it->second->get_bind_pose(temp_pose.data());

        // Evaluate the clip at current time
        clip.evaluate(layer.time, temp_pose.data(), bone_count);

        // Blend into local pose based on blend mode and weight
        float w = layer.weight;
        if (w <= 0.0f) continue;

        switch (layer.blend) {
            case AnimBlendMode::OVERRIDE:
                for (uint32_t i = 0; i < bone_count; ++i) {
                    instance.local_pose[i] = lerp_transform(
                        instance.local_pose[i], temp_pose[i], w);
                }
                break;

            case AnimBlendMode::ADDITIVE:
                for (uint32_t i = 0; i < bone_count; ++i) {
                    // Additive: add the difference from bind pose
                    Transform& dst = instance.local_pose[i];
                    const Transform& bind = skel_it->second->bone(i).bind_pose;
                    const Transform& anim = temp_pose[i];

                    dst.translation.x += (anim.translation.x - bind.translation.x) * w;
                    dst.translation.y += (anim.translation.y - bind.translation.y) * w;
                    dst.translation.z += (anim.translation.z - bind.translation.z) * w;

                    // Additive rotation
                    Quaternion diff = anim.rotation * bind.rotation.conjugate();
                    Quaternion blended = slerp(Quaternion::identity(), diff, w);
                    dst.rotation = blended * dst.rotation;
                }
                break;

            case AnimBlendMode::MULTIPLY:
                for (uint32_t i = 0; i < bone_count; ++i) {
                    Transform& dst = instance.local_pose[i];
                    const Transform& anim = temp_pose[i];

                    dst.scale.x *= 1.0f + (anim.scale.x - 1.0f) * w;
                    dst.scale.y *= 1.0f + (anim.scale.y - 1.0f) * w;
                    dst.scale.z *= 1.0f + (anim.scale.z - 1.0f) * w;
                }
                break;
        }
    }
}

void AnimationSystem::apply_ik(AnimationInstance& instance) {
    if (instance.ik_chains.empty()) return;

    auto skel_it = skeletons_.find(instance.skeleton);
    if (skel_it == skeletons_.end()) return;

    for (const auto& chain : instance.ik_chains) {
        ik_solver_.solve(*skel_it->second, chain,
                         instance.local_pose.data(),
                         instance.world_matrices.data());

        // Recompute world matrices after IK
        skel_it->second->compute_world_matrices(
            instance.local_pose.data(), instance.world_matrices.data());
    }
}

const float4x4* AnimationSystem::get_skinning_matrices(AnimationStateHandle handle) const {
    auto it = instances_.find(handle);
    if (it == instances_.end() || it->second->skinning_matrices.empty()) return nullptr;
    return it->second->skinning_matrices.data();
}

const float4x4* AnimationSystem::get_world_matrices(AnimationStateHandle handle) const {
    auto it = instances_.find(handle);
    if (it == instances_.end() || it->second->world_matrices.empty()) return nullptr;
    return it->second->world_matrices.data();
}

uint32_t AnimationSystem::get_bone_count(AnimationStateHandle handle) const {
    auto it = instances_.find(handle);
    if (it == instances_.end()) return 0;
    auto skel_it = skeletons_.find(it->second->skeleton);
    if (skel_it == skeletons_.end()) return 0;
    return skel_it->second->bone_count();
}

// ============================================================
// Resource Access
// ============================================================

const Skeleton* AnimationSystem::get_skeleton(SkeletonHandle handle) const {
    auto it = skeletons_.find(handle);
    return (it != skeletons_.end()) ? it->second.get() : nullptr;
}

const AnimationClip* AnimationSystem::get_clip(AnimationClipHandle handle) const {
    auto it = clips_.find(handle);
    return (it != clips_.end()) ? it->second.get() : nullptr;
}

const AnimationInstance* AnimationSystem::get_instance(AnimationStateHandle handle) const {
    auto it = instances_.find(handle);
    return (it != instances_.end()) ? it->second.get() : nullptr;
}

// ============================================================
// 2D / Rive / Lottie / Vector Factories
// ============================================================

Animation2DPlayer* AnimationSystem::create_2d_player() {
    anim_2d_players_.push_back(std::make_unique<Animation2DPlayer>());
    return anim_2d_players_.back().get();
}

void AnimationSystem::destroy_2d_player(Animation2DPlayer* player) {
    anim_2d_players_.erase(
        std::remove_if(anim_2d_players_.begin(), anim_2d_players_.end(),
                       [player](const auto& p) { return p.get() == player; }),
        anim_2d_players_.end());
}

RiveAnimation* AnimationSystem::create_rive_instance() {
    rive_instances_.push_back(std::make_unique<RiveAnimation>());
    return rive_instances_.back().get();
}

void AnimationSystem::destroy_rive_instance(RiveAnimation* instance) {
    rive_instances_.erase(
        std::remove_if(rive_instances_.begin(), rive_instances_.end(),
                       [instance](const auto& p) { return p.get() == instance; }),
        rive_instances_.end());
}

LottieAnimation* AnimationSystem::create_lottie_instance() {
    lottie_instances_.push_back(std::make_unique<LottieAnimation>());
    return lottie_instances_.back().get();
}

void AnimationSystem::destroy_lottie_instance(LottieAnimation* instance) {
    lottie_instances_.erase(
        std::remove_if(lottie_instances_.begin(), lottie_instances_.end(),
                       [instance](const auto& p) { return p.get() == instance; }),
        lottie_instances_.end());
}

VectorAnimationPlayer* AnimationSystem::create_vector_player() {
    vector_players_.push_back(std::make_unique<VectorAnimationPlayer>());
    return vector_players_.back().get();
}

void AnimationSystem::destroy_vector_player(VectorAnimationPlayer* player) {
    vector_players_.erase(
        std::remove_if(vector_players_.begin(), vector_players_.end(),
                       [player](const auto& p) { return p.get() == player; }),
        vector_players_.end());
}

} // namespace pictor
