#pragma once

#include "pictor/animation/animation_types.h"
#include "pictor/animation/skeleton.h"

namespace pictor {

/// Inverse Kinematics solver supporting CCD, FABRIK, and Two-Bone algorithms.
/// Modifies bone transforms in-place to reach target positions.
class IKSolver {
public:
    IKSolver() = default;
    ~IKSolver() = default;

    /// Solve IK for a chain and apply results to the local transforms.
    /// @param skeleton     The skeleton hierarchy
    /// @param chain        IK chain descriptor (target, solver params)
    /// @param local_transforms  In/out local transforms (modified in-place)
    /// @param world_matrices    Current world-space matrices (read-only for solver)
    void solve(const Skeleton& skeleton,
               const IKChainDescriptor& chain,
               Transform* local_transforms,
               const float4x4* world_matrices);

    /// Apply FK pose overrides (blend between animation and manual FK values)
    static void apply_fk_overrides(const std::vector<FKPoseOverride>& overrides,
                                   Transform* local_transforms);

private:
    /// Two-bone analytical solver (for arm/leg chains)
    void solve_two_bone(const Skeleton& skeleton,
                        const IKChainDescriptor& chain,
                        Transform* local_transforms,
                        const float4x4* world_matrices);

    /// CCD (Cyclic Coordinate Descent) iterative solver
    void solve_ccd(const Skeleton& skeleton,
                   const IKChainDescriptor& chain,
                   Transform* local_transforms,
                   const float4x4* world_matrices);

    /// FABRIK (Forward And Backward Reaching Inverse Kinematics)
    void solve_fabrik(const Skeleton& skeleton,
                      const IKChainDescriptor& chain,
                      Transform* local_transforms,
                      const float4x4* world_matrices);

    /// Helper: get bone position from world matrix
    static float3 get_bone_position(const float4x4& world_matrix);

    /// Helper: compute distance between two points
    static float distance(const float3& a, const float3& b);

    /// Helper: normalize a vector
    static float3 normalize(const float3& v);
};

} // namespace pictor
