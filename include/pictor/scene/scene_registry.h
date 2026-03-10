#pragma once

#include "pictor/core/types.h"
#include "pictor/scene/object_pool.h"
#include "pictor/scene/object_classifier.h"
#include "pictor/memory/memory_subsystem.h"
#include <unordered_map>
#include <functional>

namespace pictor {

/// Central object management (§2.2, §12).
/// Manages Object Pools, SoA stream allocation, object registration/removal,
/// and objectId → pool index reverse lookup.
class SceneRegistry {
public:
    explicit SceneRegistry(MemorySubsystem& memory);
    ~SceneRegistry();

    SceneRegistry(const SceneRegistry&) = delete;
    SceneRegistry& operator=(const SceneRegistry&) = delete;

    // ---- Object Operations (§12 Public API) ----

    /// Register an object. ObjectClassifier determines pool placement.
    /// Returns a unique ObjectId.
    ObjectId register_object(const ObjectDescriptor& desc);

    /// Unregister an object by id. Uses swap-and-pop removal.
    void unregister_object(ObjectId id);

    /// Update transform for a Dynamic Pool object
    void update_transform(ObjectId id, const float4x4& transform);

    /// Update bounds for an object
    void update_bounds(ObjectId id, const AABB& bounds);

    /// Move object between pools (§4.1 Pool migration)
    void change_pool(ObjectId id, PoolType new_pool);

    // ---- Compute Update (§12) ----

    /// Associate a compute update shader with a GPU Driven pool
    void set_compute_update_shader(ShaderHandle shader);
    ShaderHandle compute_update_shader() const { return compute_shader_; }

    // ---- Pool Access ----

    ObjectPool& static_pool()      { return pools_[static_cast<int>(PoolType::STATIC)]; }
    ObjectPool& dynamic_pool()     { return pools_[static_cast<int>(PoolType::DYNAMIC)]; }
    ObjectPool& gpu_driven_pool()  { return pools_[static_cast<int>(PoolType::GPU_DRIVEN)]; }

    const ObjectPool& static_pool()     const { return pools_[static_cast<int>(PoolType::STATIC)]; }
    const ObjectPool& dynamic_pool()    const { return pools_[static_cast<int>(PoolType::DYNAMIC)]; }
    const ObjectPool& gpu_driven_pool() const { return pools_[static_cast<int>(PoolType::GPU_DRIVEN)]; }

    ObjectPool& pool(PoolType type) { return pools_[static_cast<int>(type)]; }
    const ObjectPool& pool(PoolType type) const { return pools_[static_cast<int>(type)]; }

    // ---- Query ----

    /// Look up which pool and index an object is in
    struct ObjectLocation {
        PoolType pool_type;
        uint32_t pool_index;
        bool     valid = false;
    };

    ObjectLocation find_object(ObjectId id) const;

    uint32_t total_object_count() const;
    uint32_t next_object_id() const { return next_id_; }

    // ---- Iteration ----

    /// Execute a callback for each pool
    void for_each_pool(std::function<void(ObjectPool&, PoolType)> fn);

private:
    ObjectPool pools_[3]; // indexed by PoolType

    /// objectId → (PoolType, pool-local index) reverse lookup (§4.1)
    struct PoolIndex {
        PoolType pool_type;
        uint32_t index;
    };
    std::unordered_map<ObjectId, PoolIndex> id_map_;

    ObjectId     next_id_        = 0;
    ShaderHandle compute_shader_ = INVALID_MESH;
    MemorySubsystem& memory_;
};

} // namespace pictor
