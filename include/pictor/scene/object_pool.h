#pragma once

#include "pictor/core/types.h"
#include "pictor/scene/soa_stream.h"
#include "pictor/memory/pool_allocator.h"
#include <unordered_map>

namespace pictor {

/// Physically separated SoA stream group for one object category (§4.1).
/// Each pool (Static/Dynamic/GPU Driven) has independent SoA arrays
/// that can be traversed front-to-back without branching.
class ObjectPool {
public:
    ObjectPool() = default;

    void init(PoolType type, PoolAllocator* allocator, size_t initial_capacity = 1024);

    // ---- Registration ----

    /// Add an object, returns the pool-local index
    uint32_t add(const ObjectDescriptor& desc, ObjectId id);

    /// Remove by pool-local index using swap-and-pop
    /// Returns the ObjectId that was swapped into `index` (or INVALID if it was the last)
    ObjectId remove(uint32_t index);

    // ---- SoA Stream Access (§3.2) ----

    // Stream Group A: Culling (Hot)
    SoAStream<AABB>&     bounds()           { return bounds_; }
    SoAStream<uint8_t>&  visibility_flags() { return visibility_flags_; }

    // Stream Group B: Sort/Batch (Hot)
    SoAStream<uint64_t>& shader_keys()      { return shader_keys_; }
    SoAStream<uint64_t>& sort_keys()        { return sort_keys_; }
    SoAStream<uint32_t>& material_keys()    { return material_keys_; }

    // Stream Group C: Transform (Hot for Dynamic)
    SoAStream<float4x4>& transforms()       { return transforms_; }
    SoAStream<float4x4>& prev_transforms()  { return prev_transforms_; }

    // Stream Group D: Metadata (Cold)
    SoAStream<MeshHandle>&     mesh_handles()     { return mesh_handles_; }
    SoAStream<MaterialHandle>& material_handles() { return material_handles_; }
    SoAStream<uint8_t>&        lod_levels()       { return lod_levels_; }
    SoAStream<uint16_t>&       flags()            { return flags_; }
    SoAStream<uint32_t>&       last_frame_updated() { return last_frame_updated_; }

    // Stream Group: ID mapping
    SoAStream<ObjectId>&       object_ids()       { return object_ids_; }

    // ---- Properties ----

    PoolType type() const { return type_; }
    uint32_t count() const { return static_cast<uint32_t>(bounds_.size()); }
    bool     empty() const { return bounds_.empty(); }

    // Const access
    const SoAStream<AABB>&         bounds()          const { return bounds_; }
    const SoAStream<float4x4>&     transforms()      const { return transforms_; }
    const SoAStream<uint64_t>&     shader_keys()     const { return shader_keys_; }
    const SoAStream<uint32_t>&     material_keys()   const { return material_keys_; }
    const SoAStream<ObjectId>&     object_ids()      const { return object_ids_; }
    const SoAStream<MeshHandle>&   mesh_handles()    const { return mesh_handles_; }
    const SoAStream<uint16_t>&     flags()           const { return flags_; }

private:
    PoolType type_ = PoolType::DYNAMIC;

    // Stream Group A: Culling (Hot)
    SoAStream<AABB>     bounds_;
    SoAStream<uint8_t>  visibility_flags_;

    // Stream Group B: Sort/Batch (Hot)
    SoAStream<uint64_t> shader_keys_;
    SoAStream<uint64_t> sort_keys_;
    SoAStream<uint32_t> material_keys_;

    // Stream Group C: Transform (Hot for Dynamic)
    SoAStream<float4x4> transforms_;
    SoAStream<float4x4> prev_transforms_;

    // Stream Group D: Metadata (Cold)
    SoAStream<MeshHandle>     mesh_handles_;
    SoAStream<MaterialHandle> material_handles_;
    SoAStream<uint8_t>        lod_levels_;
    SoAStream<uint16_t>       flags_;
    SoAStream<uint32_t>       last_frame_updated_;

    // ID mapping
    SoAStream<ObjectId>       object_ids_;
};

} // namespace pictor
