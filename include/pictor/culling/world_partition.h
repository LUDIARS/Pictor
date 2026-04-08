#pragma once

#include "pictor/core/types.h"
#include <cstdint>
#include <cmath>
#include <vector>
#include <unordered_map>

namespace pictor {

// ============================================================
// World Partition Grid (§10.3)
//
// Divides world space into a uniform 3D grid.
// Each object is assigned to a cell based on its AABB center.
// Used as broad-phase in the culling pipeline:
//   Broad phase: frustum vs. cell AABB (skip entire cells)
//   Narrow phase: frustum vs. object AABB (per-object in active cells)
//
// Cell key encoding:
//   key = (ix & MASK) | ((iy & MASK) << BITS) | ((iz & MASK) << (BITS*2))
//   where ix = floor((pos.x - origin.x) / cell_size)
// ============================================================

/// Configuration for world partition grid.
struct WorldPartitionConfig {
    float3   origin    = {0.0f, 0.0f, 0.0f};   // World origin (minimum corner)
    float    world_size = 10000.0f;              // World extent per axis
    float    cell_size  = 100.0f;                // Partition cell size per axis
};

/// A cell key identifying a grid cell.
using CellKey = uint64_t;

/// Stores objects within a single partition cell.
struct PartitionCell {
    AABB                   bounds;       // Union of all object AABBs in this cell
    std::vector<uint32_t>  object_indices; // Indices into the pool
};

/// Spatial partition grid for broad-phase culling.
class WorldPartition {
public:
    WorldPartition() = default;

    /// Initialize with configuration.
    void configure(const WorldPartitionConfig& config);

    /// Rebuild partition from scratch using object bounds.
    /// Call after scene changes or periodically.
    void rebuild(const AABB* bounds, uint32_t count);

    /// Assign a single object to its cell (incremental update).
    void assign_object(uint32_t index, const AABB& aabb);

    /// Remove an object from its current cell.
    void remove_object(uint32_t index);

    /// Clear all cells.
    void clear();

    /// Broad-phase frustum query: returns cell keys whose bounds intersect the frustum.
    /// Writes active cell pointers to `out_cells`, returns count.
    uint32_t query_frustum(const Frustum& frustum,
                           const PartitionCell** out_cells,
                           uint32_t max_cells) const;

    /// Compute cell key from world position.
    CellKey cell_key_from_position(const float3& pos) const;

    /// Compute AABB for a given cell key.
    AABB cell_bounds(CellKey key) const;

    /// Get all active cells.
    const std::unordered_map<CellKey, PartitionCell>& cells() const { return cells_; }

    /// Statistics.
    uint32_t cell_count() const { return static_cast<uint32_t>(cells_.size()); }
    uint32_t grid_divisions() const { return grid_divisions_; }

private:
    /// Compute grid indices from world position.
    void position_to_indices(const float3& pos, int32_t& ix, int32_t& iy, int32_t& iz) const;

    /// Encode grid indices to cell key.
    static CellKey encode_key(int32_t ix, int32_t iy, int32_t iz);

    /// Decode cell key to grid indices.
    static void decode_key(CellKey key, int32_t& ix, int32_t& iy, int32_t& iz);

    WorldPartitionConfig config_;
    uint32_t             grid_divisions_ = 0;    // world_size / cell_size
    float                inv_cell_size_  = 0.0f;  // 1.0 / cell_size

    std::unordered_map<CellKey, PartitionCell> cells_;

    // Reverse lookup: object_index → cell_key (for removal/update)
    std::unordered_map<uint32_t, CellKey> object_cell_map_;
};

} // namespace pictor
