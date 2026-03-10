#pragma once

#include "pictor/core/types.h"
#include "pictor/memory/pool_allocator.h"
#include <vector>

namespace pictor {

/// Flat BVH node — 32 bytes, 2 nodes per cache line (§10.2).
/// Pointerless; children referenced by array index.
struct alignas(32) BVHNode {
    float3   aabb_min;
    uint32_t child_or_object_index; // internal: left child / leaf: object index
    float3   aabb_max;
    uint32_t flags; // bit0: isLeaf, bit1-7: objectCount

    bool is_leaf() const { return flags & 1; }
    uint32_t object_count() const { return (flags >> 1) & 0x7F; }

    static uint32_t make_flags(bool leaf, uint32_t count) {
        return (leaf ? 1u : 0u) | ((count & 0x7F) << 1);
    }
};

static_assert(sizeof(BVHNode) == 32, "BVHNode must be 32 bytes");

/// Flat BVH with Van Emde Boas layout for cache-optimal traversal (§10.2).
/// SAH construction for static, refit for dynamic with async rebuild threshold.
class FlatBVH {
public:
    FlatBVH() = default;
    ~FlatBVH() = default;

    /// Build BVH from AABB array using SAH (Surface Area Heuristic)
    void build(const AABB* bounds, const uint32_t* indices, uint32_t count,
               PoolAllocator& allocator);

    /// Refit existing BVH with updated bounds (for dynamic objects)
    void refit(const AABB* bounds);

    /// Check if refit quality has degraded enough to warrant rebuild
    bool needs_rebuild(float quality_threshold = 2.0f) const;

    /// Query: find all objects intersecting the frustum
    /// Writes visible indices to `out_visible`, returns count
    uint32_t query_frustum(const Frustum& frustum,
                           uint32_t* out_visible, uint32_t max_results) const;

    /// Query: find all objects intersecting an AABB
    uint32_t query_aabb(const AABB& query,
                        uint32_t* out_results, uint32_t max_results) const;

    const BVHNode* nodes() const { return nodes_; }
    uint32_t node_count() const { return node_count_; }
    bool empty() const { return node_count_ == 0; }

private:
    struct BuildEntry {
        uint32_t parent_index;
        uint32_t start;
        uint32_t end;
        bool     is_right;
    };

    /// SAH-based split finding
    uint32_t find_best_split(const AABB* bounds, const uint32_t* indices,
                             uint32_t start, uint32_t end, float& best_cost) const;

    /// Rearrange nodes into Van Emde Boas layout (§10.2)
    void apply_veb_layout(PoolAllocator& allocator);

    /// Estimate BVH quality cost
    float best_cost_estimate() const;

    BVHNode*  nodes_      = nullptr;
    uint32_t* leaf_indices_ = nullptr; // object indices for leaves
    uint32_t  node_count_ = 0;
    float     initial_cost_ = 0.0f;
    float     current_cost_ = 0.0f;
};

} // namespace pictor
