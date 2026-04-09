#include "pictor/culling/world_partition.h"
#include <cstring>
#include <cstdio>

namespace pictor {

// Bit layout for cell key encoding.
// 21 bits per axis → supports up to 2^21 = 2M divisions per axis.
static constexpr int32_t KEY_BITS = 21;
static constexpr int32_t KEY_MASK = (1 << KEY_BITS) - 1;

void WorldPartition::configure(const WorldPartitionConfig& config) {
    config_ = config;

    if (config_.cell_size <= 0.0f) {
        fprintf(stderr, "[WorldPartition] Invalid cell_size (%.2f), defaulting to 100\n",
                config_.cell_size);
        config_.cell_size = 100.0f;
    }

    inv_cell_size_ = 1.0f / config_.cell_size;
    grid_divisions_ = static_cast<uint32_t>(
        std::ceil(config_.world_size / config_.cell_size));

    printf("[WorldPartition] Configured: world_size=%.0f cell_size=%.0f divisions=%u\n",
           config_.world_size, config_.cell_size, grid_divisions_);
}

void WorldPartition::rebuild(const AABB* bounds, uint32_t count) {
    clear();

    for (uint32_t i = 0; i < count; ++i) {
        assign_object(i, bounds[i]);
    }
}

void WorldPartition::assign_object(uint32_t index, const AABB& aabb) {
    // Remove from previous cell if re-assigning
    auto prev = object_cell_map_.find(index);
    if (prev != object_cell_map_.end()) {
        auto cell_it = cells_.find(prev->second);
        if (cell_it != cells_.end()) {
            auto& indices = cell_it->second.object_indices;
            for (auto it = indices.begin(); it != indices.end(); ++it) {
                if (*it == index) {
                    indices.erase(it);
                    break;
                }
            }
            if (indices.empty()) {
                cells_.erase(cell_it);
            }
        }
    }

    // Compute cell from AABB center
    float3 center = aabb.center();
    CellKey key = cell_key_from_position(center);

    auto& cell = cells_[key];
    cell.object_indices.push_back(index);

    // Expand cell bounds
    if (cell.object_indices.size() == 1) {
        cell.bounds = aabb;
    } else {
        cell.bounds = cell.bounds.merge(aabb);
    }

    object_cell_map_[index] = key;
}

void WorldPartition::remove_object(uint32_t index) {
    auto it = object_cell_map_.find(index);
    if (it == object_cell_map_.end()) return;

    CellKey key = it->second;
    object_cell_map_.erase(it);

    auto cell_it = cells_.find(key);
    if (cell_it == cells_.end()) return;

    auto& indices = cell_it->second.object_indices;
    for (auto vi = indices.begin(); vi != indices.end(); ++vi) {
        if (*vi == index) {
            indices.erase(vi);
            break;
        }
    }

    if (indices.empty()) {
        cells_.erase(cell_it);
    }
    // Note: cell bounds are not shrunk on removal (conservative).
    // A rebuild will fix this.
}

void WorldPartition::clear() {
    cells_.clear();
    object_cell_map_.clear();
}

uint32_t WorldPartition::query_frustum(const Frustum& frustum,
                                        const PartitionCell** out_cells,
                                        uint32_t max_cells) const {
    uint32_t count = 0;

    for (auto& [key, cell] : cells_) {
        if (count >= max_cells) break;

        // Broad phase: test cell AABB against frustum
        if (frustum.test_aabb(cell.bounds)) {
            out_cells[count++] = &cell;
        }
    }

    return count;
}

CellKey WorldPartition::cell_key_from_position(const float3& pos) const {
    int32_t ix, iy, iz;
    position_to_indices(pos, ix, iy, iz);
    return encode_key(ix, iy, iz);
}

AABB WorldPartition::cell_bounds(CellKey key) const {
    int32_t ix, iy, iz;
    decode_key(key, ix, iy, iz);

    AABB aabb;
    aabb.min.x = config_.origin.x + static_cast<float>(ix) * config_.cell_size;
    aabb.min.y = config_.origin.y + static_cast<float>(iy) * config_.cell_size;
    aabb.min.z = config_.origin.z + static_cast<float>(iz) * config_.cell_size;
    aabb.max.x = aabb.min.x + config_.cell_size;
    aabb.max.y = aabb.min.y + config_.cell_size;
    aabb.max.z = aabb.min.z + config_.cell_size;
    return aabb;
}

void WorldPartition::position_to_indices(const float3& pos,
                                          int32_t& ix, int32_t& iy, int32_t& iz) const {
    ix = static_cast<int32_t>(std::floor((pos.x - config_.origin.x) * inv_cell_size_));
    iy = static_cast<int32_t>(std::floor((pos.y - config_.origin.y) * inv_cell_size_));
    iz = static_cast<int32_t>(std::floor((pos.z - config_.origin.z) * inv_cell_size_));
}

CellKey WorldPartition::encode_key(int32_t ix, int32_t iy, int32_t iz) {
    return static_cast<CellKey>(ix & KEY_MASK)
         | (static_cast<CellKey>(iy & KEY_MASK) << KEY_BITS)
         | (static_cast<CellKey>(iz & KEY_MASK) << (KEY_BITS * 2));
}

void WorldPartition::decode_key(CellKey key, int32_t& ix, int32_t& iy, int32_t& iz) {
    ix = static_cast<int32_t>(key & KEY_MASK);
    iy = static_cast<int32_t>((key >> KEY_BITS) & KEY_MASK);
    iz = static_cast<int32_t>((key >> (KEY_BITS * 2)) & KEY_MASK);

    // Sign-extend for negative indices
    if (ix & (1 << (KEY_BITS - 1))) ix |= ~KEY_MASK;
    if (iy & (1 << (KEY_BITS - 1))) iy |= ~KEY_MASK;
    if (iz & (1 << (KEY_BITS - 1))) iz |= ~KEY_MASK;
}

} // namespace pictor
