#include "pictor/scene/object_pool.h"

namespace pictor {

void ObjectPool::init(PoolType type, PoolAllocator* allocator, size_t initial_capacity) {
    type_ = type;

    // Stream Group A: Culling (Hot — §3.2)
    bounds_.init(allocator, initial_capacity);
    visibility_flags_.init(allocator, initial_capacity);

    // Stream Group B: Sort/Batch (Hot — §3.2)
    shader_keys_.init(allocator, initial_capacity);
    sort_keys_.init(allocator, initial_capacity);
    material_keys_.init(allocator, initial_capacity);

    // Stream Group C: Transform (Hot for Dynamic — §3.2)
    transforms_.init(allocator, initial_capacity);
    prev_transforms_.init(allocator, initial_capacity);

    // Stream Group D: Metadata (Cold — §3.2)
    mesh_handles_.init(allocator, initial_capacity);
    material_handles_.init(allocator, initial_capacity);
    lod_levels_.init(allocator, initial_capacity);
    flags_.init(allocator, initial_capacity);
    last_frame_updated_.init(allocator, initial_capacity);

    // ID mapping
    object_ids_.init(allocator, initial_capacity);
}

uint32_t ObjectPool::add(const ObjectDescriptor& desc, ObjectId id) {
    // All streams push_back in parallel — same index across all (§3.1)
    uint32_t index = bounds_.push_back(desc.bounds);
    visibility_flags_.push_back(1); // visible by default

    shader_keys_.push_back(desc.shaderKey);
    sort_keys_.push_back(0); // computed during batch build
    material_keys_.push_back(desc.materialKey);

    transforms_.push_back(desc.transform);
    prev_transforms_.push_back(desc.transform);

    mesh_handles_.push_back(desc.mesh);
    material_handles_.push_back(desc.material);
    lod_levels_.push_back(desc.lodLevel);
    flags_.push_back(desc.flags);
    last_frame_updated_.push_back(0);

    object_ids_.push_back(id);

    return index;
}

ObjectId ObjectPool::remove(uint32_t index) {
    ObjectId swapped_id = INVALID_OBJECT_ID;
    uint32_t last = count() - 1;

    if (index < last) {
        // Record which object was swapped into this slot
        swapped_id = object_ids_[last];
    }

    // Swap-and-pop all streams (§4.1)
    bounds_.swap_and_pop(index);
    visibility_flags_.swap_and_pop(index);
    shader_keys_.swap_and_pop(index);
    sort_keys_.swap_and_pop(index);
    material_keys_.swap_and_pop(index);
    transforms_.swap_and_pop(index);
    prev_transforms_.swap_and_pop(index);
    mesh_handles_.swap_and_pop(index);
    material_handles_.swap_and_pop(index);
    lod_levels_.swap_and_pop(index);
    flags_.swap_and_pop(index);
    last_frame_updated_.swap_and_pop(index);
    object_ids_.swap_and_pop(index);

    return swapped_id;
}

} // namespace pictor
