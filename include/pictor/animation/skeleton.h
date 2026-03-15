#pragma once

#include "pictor/animation/animation_types.h"
#include <vector>
#include <string>
#include <unordered_map>

namespace pictor {

/// Skeleton manages a bone hierarchy for skeletal animation.
/// Supports FK pose evaluation and world-space matrix computation.
class Skeleton {
public:
    Skeleton() = default;
    explicit Skeleton(const SkeletonDescriptor& desc);
    ~Skeleton() = default;

    /// Compute world-space matrices from local transforms.
    /// @param local_transforms  Local bone transforms (one per bone)
    /// @param out_world_matrices  Output world-space matrices (one per bone)
    void compute_world_matrices(const Transform* local_transforms,
                                float4x4* out_world_matrices) const;

    /// Compute final skinning matrices (world * inverse_bind).
    /// @param local_transforms  Local bone transforms
    /// @param out_skinning_matrices  Output matrices ready for GPU skinning
    void compute_skinning_matrices(const Transform* local_transforms,
                                   float4x4* out_skinning_matrices) const;

    /// Get bind pose transforms
    void get_bind_pose(Transform* out_transforms) const;

    /// Find bone index by name, returns -1 if not found
    int32_t find_bone(const std::string& name) const;

    uint32_t bone_count() const { return static_cast<uint32_t>(bones_.size()); }
    const Bone& bone(uint32_t index) const { return bones_[index]; }
    const std::string& name() const { return name_; }
    const std::vector<Bone>& bones() const { return bones_; }

private:
    std::string        name_;
    std::vector<Bone>  bones_;
    std::unordered_map<std::string, int32_t> bone_name_map_;
};

} // namespace pictor
