#include "pictor/scene/scene_registry.h"

namespace pictor {

SceneRegistry::SceneRegistry(MemorySubsystem& memory)
    : memory_(memory)
{
    // Initialize all three pools (§4.1)
    pools_[static_cast<int>(PoolType::STATIC)].init(
        PoolType::STATIC, &memory.pool_allocator());
    pools_[static_cast<int>(PoolType::DYNAMIC)].init(
        PoolType::DYNAMIC, &memory.pool_allocator());
    pools_[static_cast<int>(PoolType::GPU_DRIVEN)].init(
        PoolType::GPU_DRIVEN, &memory.pool_allocator());
}

SceneRegistry::~SceneRegistry() = default;

ObjectId SceneRegistry::register_object(const ObjectDescriptor& desc) {
    // Classify the object (§2.2)
    auto result = ObjectClassifier::classify(desc);

    ObjectId id = next_id_++;

    // Create modified descriptor with adjusted flags
    ObjectDescriptor adjusted = desc;
    adjusted.flags = result.adjusted_flags;

    // Add to the appropriate pool (§4.1)
    ObjectPool& pool = pools_[static_cast<int>(result.pool_type)];
    uint32_t pool_index = pool.add(adjusted, id);

    // Store reverse lookup (§4.1)
    id_map_[id] = {result.pool_type, pool_index};

    return id;
}

void SceneRegistry::unregister_object(ObjectId id) {
    auto it = id_map_.find(id);
    if (it == id_map_.end()) return;

    PoolType pool_type = it->second.pool_type;
    uint32_t pool_index = it->second.index;
    ObjectPool& pool = pools_[static_cast<int>(pool_type)];

    // Swap-and-pop removal (§4.1)
    ObjectId swapped_id = pool.remove(pool_index);

    // Update the swapped object's index in the map
    if (swapped_id != INVALID_OBJECT_ID) {
        id_map_[swapped_id].index = pool_index;
    }

    // Remove the deleted object from the map
    id_map_.erase(it);
}

void SceneRegistry::update_transform(ObjectId id, const float4x4& transform) {
    auto it = id_map_.find(id);
    if (it == id_map_.end()) return;

    ObjectPool& pool = pools_[static_cast<int>(it->second.pool_type)];
    uint32_t idx = it->second.index;

    pool.transforms()[idx] = transform;
    pool.last_frame_updated()[idx] = memory_.frame_number();
}

void SceneRegistry::update_bounds(ObjectId id, const AABB& bounds) {
    auto it = id_map_.find(id);
    if (it == id_map_.end()) return;

    ObjectPool& pool = pools_[static_cast<int>(it->second.pool_type)];
    pool.bounds()[it->second.index] = bounds;
}

void SceneRegistry::change_pool(ObjectId id, PoolType new_pool) {
    auto it = id_map_.find(id);
    if (it == id_map_.end()) return;

    PoolType old_pool_type = it->second.pool_type;
    if (old_pool_type == new_pool) return;

    uint32_t old_index = it->second.index;
    ObjectPool& old_pool = pools_[static_cast<int>(old_pool_type)];

    // Collect current data before removal
    ObjectDescriptor desc;
    desc.bounds      = old_pool.bounds()[old_index];
    desc.transform   = old_pool.transforms()[old_index];
    desc.mesh        = old_pool.mesh_handles()[old_index];
    desc.material    = old_pool.material_handles()[old_index];
    desc.shaderKey   = old_pool.shader_keys()[old_index];
    desc.materialKey = old_pool.material_keys()[old_index];
    desc.lodLevel    = old_pool.lod_levels()[old_index];
    desc.flags       = old_pool.flags()[old_index];

    // Remove from old pool
    ObjectId swapped_id = old_pool.remove(old_index);
    if (swapped_id != INVALID_OBJECT_ID) {
        id_map_[swapped_id].index = old_index;
    }

    // Set new pool flag
    desc.flags &= ~(ObjectFlags::STATIC | ObjectFlags::DYNAMIC | ObjectFlags::GPU_DRIVEN);
    switch (new_pool) {
        case PoolType::STATIC:    desc.flags |= ObjectFlags::STATIC; break;
        case PoolType::DYNAMIC:   desc.flags |= ObjectFlags::DYNAMIC; break;
        case PoolType::GPU_DRIVEN: desc.flags |= ObjectFlags::GPU_DRIVEN; break;
    }

    // Add to new pool
    ObjectPool& target_pool = pools_[static_cast<int>(new_pool)];
    uint32_t new_index = target_pool.add(desc, id);

    // Update reverse lookup
    id_map_[id] = {new_pool, new_index};
}

void SceneRegistry::set_compute_update_shader(ShaderHandle shader) {
    compute_shader_ = shader;
}

SceneRegistry::ObjectLocation SceneRegistry::find_object(ObjectId id) const {
    auto it = id_map_.find(id);
    if (it == id_map_.end()) {
        return {PoolType::DYNAMIC, 0, false};
    }
    return {it->second.pool_type, it->second.index, true};
}

uint32_t SceneRegistry::total_object_count() const {
    uint32_t total = 0;
    for (int i = 0; i < 3; ++i) {
        total += pools_[i].count();
    }
    return total;
}

void SceneRegistry::for_each_pool(std::function<void(ObjectPool&, PoolType)> fn) {
    fn(pools_[0], PoolType::STATIC);
    fn(pools_[1], PoolType::DYNAMIC);
    fn(pools_[2], PoolType::GPU_DRIVEN);
}

} // namespace pictor
