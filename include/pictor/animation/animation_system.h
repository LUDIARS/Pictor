#pragma once

#include "pictor/animation/animation_types.h"
#include "pictor/animation/animation_clip.h"
#include "pictor/animation/skeleton.h"
#include "pictor/animation/ik_solver.h"
#include "pictor/animation/motion_estimator.h"
#include "pictor/animation/animation_2d.h"
#include "pictor/animation/rive_animation.h"
#include "pictor/animation/lottie_animation.h"
#include "pictor/animation/vector_animation.h"
#include <unordered_map>
#include <memory>
#include <vector>

namespace pictor {

/// Configuration for the animation system
struct AnimationSystemConfig {
    uint32_t max_skeletons       = 256;
    uint32_t max_clips           = 1024;
    uint32_t max_active_instances = 512;
    uint32_t max_bones_per_skeleton = 256;
    bool     gpu_skinning_enabled = true;
};

/// An active animation instance bound to a scene object
struct AnimationInstance {
    ObjectId               object_id   = INVALID_OBJECT_ID;
    SkeletonHandle         skeleton    = INVALID_SKELETON;
    std::vector<AnimationPlayback> layers;   // Animation blend layers
    std::vector<IKChainDescriptor> ik_chains;
    std::vector<FKPoseOverride>    fk_overrides;

    // Runtime pose data (allocated per-instance)
    std::vector<Transform> local_pose;
    std::vector<float4x4>  world_matrices;
    std::vector<float4x4>  skinning_matrices;
};

/// Main animation system — orchestrates all animation subsystems.
/// Manages skeletons, clips, animation instances, IK/FK, and 2D/vector animations.
/// Integrates with PictorRenderer via the update loop.
class AnimationSystem {
public:
    AnimationSystem() = default;
    explicit AnimationSystem(const AnimationSystemConfig& config);
    ~AnimationSystem();

    AnimationSystem(const AnimationSystem&) = delete;
    AnimationSystem& operator=(const AnimationSystem&) = delete;

    /// Initialize the animation system
    void initialize(const AnimationSystemConfig& config);

    /// Shutdown and release all resources
    void shutdown();

    bool is_initialized() const { return initialized_; }

    // ============================================================
    // Resource Registration
    // ============================================================

    /// Register a skeleton and return its handle
    SkeletonHandle register_skeleton(const SkeletonDescriptor& desc);

    /// Unregister a skeleton
    void unregister_skeleton(SkeletonHandle handle);

    /// Register an animation clip and return its handle
    AnimationClipHandle register_clip(const AnimationClipDescriptor& desc);

    /// Unregister an animation clip
    void unregister_clip(AnimationClipHandle handle);

    // ============================================================
    // Animation Instance Management
    // ============================================================

    /// Create an animation instance bound to a scene object.
    /// @param object_id  The ObjectId in PictorRenderer's scene
    /// @param skeleton   The skeleton to use (INVALID_SKELETON for non-skeletal)
    /// @return  Handle to the animation state
    AnimationStateHandle create_instance(ObjectId object_id, SkeletonHandle skeleton);

    /// Destroy an animation instance
    void destroy_instance(AnimationStateHandle handle);

    /// Play a clip on an animation instance (adds to blend layers)
    void play(AnimationStateHandle handle, AnimationClipHandle clip,
              float weight = 1.0f, float speed = 1.0f,
              AnimBlendMode blend = AnimBlendMode::OVERRIDE);

    /// Stop a specific clip on an instance
    void stop(AnimationStateHandle handle, AnimationClipHandle clip);

    /// Stop all clips on an instance
    void stop_all(AnimationStateHandle handle);

    /// Set blend weight for a specific layer
    void set_layer_weight(AnimationStateHandle handle, uint32_t layer, float weight);

    /// Set playback speed for a specific layer
    void set_layer_speed(AnimationStateHandle handle, uint32_t layer, float speed);

    // ============================================================
    // IK / FK
    // ============================================================

    /// Add an IK chain to an animation instance
    void add_ik_chain(AnimationStateHandle handle, const IKChainDescriptor& chain);

    /// Remove all IK chains from an instance
    void clear_ik_chains(AnimationStateHandle handle);

    /// Update an IK target position
    void set_ik_target(AnimationStateHandle handle, uint32_t chain_index,
                       const float3& position, const Quaternion& rotation = Quaternion::identity());

    /// Set an FK override for a specific bone
    void set_fk_override(AnimationStateHandle handle, uint32_t bone_index,
                         const Transform& local_transform, float blend_weight = 1.0f);

    /// Clear all FK overrides for an instance
    void clear_fk_overrides(AnimationStateHandle handle);

    // ============================================================
    // Motion Estimation
    // ============================================================

    /// Estimate motion distance for a registered clip
    MotionDistanceResult estimate_motion(AnimationClipHandle clip,
                                          SkeletonHandle skeleton,
                                          float sample_rate = 0.0f) const;

    // ============================================================
    // Update (called per frame)
    // ============================================================

    /// Update all animation instances.
    /// Evaluates clips, applies IK/FK, computes skinning matrices.
    /// @param delta_time  Frame delta time in seconds
    void update(float delta_time);

    /// Get the skinning matrices for an instance (for GPU upload).
    /// Returns nullptr if the instance has no skeleton.
    const float4x4* get_skinning_matrices(AnimationStateHandle handle) const;

    /// Get the number of bones for an instance's skeleton
    uint32_t get_bone_count(AnimationStateHandle handle) const;

    // ============================================================
    // 2D Animation
    // ============================================================

    /// Create a 2D animation player
    Animation2DPlayer* create_2d_player();

    /// Destroy a 2D animation player
    void destroy_2d_player(Animation2DPlayer* player);

    // ============================================================
    // Rive Animation
    // ============================================================

    /// Create a Rive animation instance
    RiveAnimation* create_rive_instance();

    /// Destroy a Rive animation instance
    void destroy_rive_instance(RiveAnimation* instance);

    // ============================================================
    // Lottie Animation
    // ============================================================

    /// Create a Lottie animation instance
    LottieAnimation* create_lottie_instance();

    /// Destroy a Lottie animation instance
    void destroy_lottie_instance(LottieAnimation* instance);

    // ============================================================
    // Vector Animation
    // ============================================================

    /// Create a vector animation player
    VectorAnimationPlayer* create_vector_player();

    /// Destroy a vector animation player
    void destroy_vector_player(VectorAnimationPlayer* player);

    // ============================================================
    // Resource Access
    // ============================================================

    const Skeleton* get_skeleton(SkeletonHandle handle) const;
    const AnimationClip* get_clip(AnimationClipHandle handle) const;
    const AnimationInstance* get_instance(AnimationStateHandle handle) const;

    uint32_t active_instance_count() const { return active_instance_count_; }
    const AnimationSystemConfig& config() const { return config_; }

private:
    /// Evaluate a single animation instance
    void evaluate_instance(AnimationInstance& instance, float delta_time);

    /// Blend multiple animation layers into local pose
    void blend_layers(AnimationInstance& instance);

    /// Apply IK chains after animation evaluation
    void apply_ik(AnimationInstance& instance);

    bool initialized_ = false;
    AnimationSystemConfig config_;

    // Registered resources
    std::unordered_map<SkeletonHandle, std::unique_ptr<Skeleton>> skeletons_;
    std::unordered_map<AnimationClipHandle, std::unique_ptr<AnimationClip>> clips_;
    std::unordered_map<AnimationStateHandle, std::unique_ptr<AnimationInstance>> instances_;

    // Handle generation
    uint32_t next_skeleton_handle_ = 0;
    uint32_t next_clip_handle_     = 0;
    uint32_t next_instance_handle_ = 0;
    uint32_t active_instance_count_ = 0;

    // Subsystems
    IKSolver        ik_solver_;
    MotionEstimator motion_estimator_;

    // Managed 2D/Rive/Lottie/Vector instances
    std::vector<std::unique_ptr<Animation2DPlayer>>      anim_2d_players_;
    std::vector<std::unique_ptr<RiveAnimation>>          rive_instances_;
    std::vector<std::unique_ptr<LottieAnimation>>        lottie_instances_;
    std::vector<std::unique_ptr<VectorAnimationPlayer>>  vector_players_;
};

} // namespace pictor
