#include "pictor/culling/flat_bvh.h"
#include <algorithm>
#include <numeric>
#include <cstring>
#include <cmath>
#include <stack>

namespace pictor {

void FlatBVH::build(const AABB* bounds, const uint32_t* indices, uint32_t count,
                    PoolAllocator& allocator) {
    if (count == 0) {
        nodes_ = nullptr;
        node_count_ = 0;
        return;
    }

    // Maximum nodes in a binary BVH: 2*N - 1
    uint32_t max_nodes = 2 * count - 1;
    nodes_ = static_cast<BVHNode*>(allocator.allocate(max_nodes * sizeof(BVHNode)));

    // Copy indices for in-place partitioning
    leaf_indices_ = static_cast<uint32_t*>(allocator.allocate(count * sizeof(uint32_t)));
    std::memcpy(leaf_indices_, indices, count * sizeof(uint32_t));

    node_count_ = 0;

    // Iterative SAH build using a stack
    struct BuildTask {
        uint32_t node_idx;
        uint32_t start;
        uint32_t end;
    };

    std::stack<BuildTask> stack;

    // Create root node
    uint32_t root_idx = node_count_++;
    stack.push({root_idx, 0, count});

    while (!stack.empty()) {
        auto task = stack.top();
        stack.pop();

        uint32_t start = task.start;
        uint32_t end = task.end;
        uint32_t n = end - start;

        // Compute node AABB
        AABB node_aabb = bounds[leaf_indices_[start]];
        for (uint32_t i = start + 1; i < end; ++i) {
            node_aabb = node_aabb.merge(bounds[leaf_indices_[i]]);
        }

        BVHNode& node = nodes_[task.node_idx];
        node.aabb_min = node_aabb.min;
        node.aabb_max = node_aabb.max;

        if (n <= 4) {
            // Leaf node
            node.child_or_object_index = start;
            node.flags = BVHNode::make_flags(true, n);
            continue;
        }

        // Find best SAH split
        float best_cost = std::numeric_limits<float>::max();
        uint32_t best_axis = 0;
        uint32_t best_split = start + n / 2;

        float3 extent{
            node_aabb.max.x - node_aabb.min.x,
            node_aabb.max.y - node_aabb.min.y,
            node_aabb.max.z - node_aabb.min.z
        };

        // Try each axis, find SAH optimal split
        for (uint32_t axis = 0; axis < 3; ++axis) {
            // Sort by centroid on this axis
            std::sort(leaf_indices_ + start, leaf_indices_ + end,
                [&bounds, axis](uint32_t a, uint32_t b) {
                    float ca, cb;
                    if (axis == 0) {
                        ca = (bounds[a].min.x + bounds[a].max.x) * 0.5f;
                        cb = (bounds[b].min.x + bounds[b].max.x) * 0.5f;
                    } else if (axis == 1) {
                        ca = (bounds[a].min.y + bounds[a].max.y) * 0.5f;
                        cb = (bounds[b].min.y + bounds[b].max.y) * 0.5f;
                    } else {
                        ca = (bounds[a].min.z + bounds[a].max.z) * 0.5f;
                        cb = (bounds[b].min.z + bounds[b].max.z) * 0.5f;
                    }
                    return ca < cb;
                });

            // Evaluate SAH at multiple split points (binned SAH)
            constexpr uint32_t NUM_BINS = 12;
            uint32_t step = std::max(1u, n / NUM_BINS);

            for (uint32_t split = start + step; split < end; split += step) {
                // Compute left and right AABBs
                AABB left_aabb = bounds[leaf_indices_[start]];
                for (uint32_t i = start + 1; i < split; ++i) {
                    left_aabb = left_aabb.merge(bounds[leaf_indices_[i]]);
                }

                AABB right_aabb = bounds[leaf_indices_[split]];
                for (uint32_t i = split + 1; i < end; ++i) {
                    right_aabb = right_aabb.merge(bounds[leaf_indices_[i]]);
                }

                uint32_t left_count = split - start;
                uint32_t right_count = end - split;

                float cost = left_aabb.surface_area() * left_count +
                             right_aabb.surface_area() * right_count;

                if (cost < best_cost) {
                    best_cost = cost;
                    best_axis = axis;
                    best_split = split;
                }
            }
        }

        // Re-sort by best axis if needed
        if (best_axis != 2) { // last sort was axis 2
            std::sort(leaf_indices_ + start, leaf_indices_ + end,
                [&bounds, best_axis](uint32_t a, uint32_t b) {
                    float ca, cb;
                    if (best_axis == 0) {
                        ca = (bounds[a].min.x + bounds[a].max.x) * 0.5f;
                        cb = (bounds[b].min.x + bounds[b].max.x) * 0.5f;
                    } else {
                        ca = (bounds[a].min.y + bounds[a].max.y) * 0.5f;
                        cb = (bounds[b].min.y + bounds[b].max.y) * 0.5f;
                    }
                    return ca < cb;
                });
        }

        // Create child nodes
        uint32_t left_idx = node_count_++;
        uint32_t right_idx = node_count_++;

        node.child_or_object_index = left_idx;
        node.flags = BVHNode::make_flags(false, 0);

        stack.push({right_idx, best_split, end});
        stack.push({left_idx, start, best_split});
    }

    initial_cost_ = best_cost_estimate();
    current_cost_ = initial_cost_;

    // Apply Van Emde Boas layout for cache optimization (§10.2)
    apply_veb_layout(allocator);
}

float FlatBVH::best_cost_estimate() const {
    if (node_count_ == 0) return 0.0f;
    return nodes_[0].aabb_max.x - nodes_[0].aabb_min.x; // simplified
}

void FlatBVH::refit(const AABB* bounds) {
    if (node_count_ == 0) return;

    // Bottom-up refit: update leaf AABBs first, then propagate up
    for (int32_t i = static_cast<int32_t>(node_count_) - 1; i >= 0; --i) {
        BVHNode& node = nodes_[i];
        if (node.is_leaf()) {
            uint32_t start = node.child_or_object_index;
            uint32_t count = node.object_count();
            if (count > 0 && leaf_indices_) {
                AABB aabb = bounds[leaf_indices_[start]];
                for (uint32_t j = 1; j < count; ++j) {
                    aabb = aabb.merge(bounds[leaf_indices_[start + j]]);
                }
                node.aabb_min = aabb.min;
                node.aabb_max = aabb.max;
            }
        } else {
            uint32_t left = node.child_or_object_index;
            uint32_t right = left + 1;
            if (left < node_count_ && right < node_count_) {
                AABB left_aabb{nodes_[left].aabb_min, nodes_[left].aabb_max};
                AABB right_aabb{nodes_[right].aabb_min, nodes_[right].aabb_max};
                AABB merged = left_aabb.merge(right_aabb);
                node.aabb_min = merged.min;
                node.aabb_max = merged.max;
            }
        }
    }
}

bool FlatBVH::needs_rebuild(float quality_threshold) const {
    if (initial_cost_ <= 0.0f) return false;
    return current_cost_ / initial_cost_ > quality_threshold;
}

uint32_t FlatBVH::query_frustum(const Frustum& frustum,
                                 uint32_t* out_visible, uint32_t max_results) const {
    if (node_count_ == 0) return 0;

    uint32_t result_count = 0;

    // Iterative BVH traversal
    uint32_t stack[64];
    int stack_ptr = 0;
    stack[stack_ptr++] = 0; // root

    while (stack_ptr > 0 && result_count < max_results) {
        uint32_t node_idx = stack[--stack_ptr];
        const BVHNode& node = nodes_[node_idx];

        AABB node_aabb{node.aabb_min, node.aabb_max};
        if (!frustum.test_aabb(node_aabb)) {
            continue; // Entire subtree is outside frustum
        }

        if (node.is_leaf()) {
            uint32_t start = node.child_or_object_index;
            uint32_t count = node.object_count();
            for (uint32_t i = 0; i < count && result_count < max_results; ++i) {
                out_visible[result_count++] = leaf_indices_[start + i];
            }
        } else {
            uint32_t left = node.child_or_object_index;
            // Push right first (so left is processed first — depth-first)
            if (left + 1 < node_count_ && stack_ptr < 63) {
                stack[stack_ptr++] = left + 1;
            }
            if (left < node_count_ && stack_ptr < 63) {
                stack[stack_ptr++] = left;
            }
        }
    }

    return result_count;
}

uint32_t FlatBVH::query_aabb(const AABB& query,
                              uint32_t* out_results, uint32_t max_results) const {
    if (node_count_ == 0) return 0;

    uint32_t result_count = 0;
    uint32_t stack[64];
    int stack_ptr = 0;
    stack[stack_ptr++] = 0;

    while (stack_ptr > 0 && result_count < max_results) {
        uint32_t node_idx = stack[--stack_ptr];
        const BVHNode& node = nodes_[node_idx];

        AABB node_aabb{node.aabb_min, node.aabb_max};
        if (!query.intersects(node_aabb)) continue;

        if (node.is_leaf()) {
            uint32_t start = node.child_or_object_index;
            uint32_t count = node.object_count();
            for (uint32_t i = 0; i < count && result_count < max_results; ++i) {
                out_results[result_count++] = leaf_indices_[start + i];
            }
        } else {
            uint32_t left = node.child_or_object_index;
            if (left + 1 < node_count_ && stack_ptr < 63) stack[stack_ptr++] = left + 1;
            if (left < node_count_ && stack_ptr < 63) stack[stack_ptr++] = left;
        }
    }

    return result_count;
}

void FlatBVH::apply_veb_layout(PoolAllocator& allocator) {
    // §10.2: Van Emde Boas layout rearranges nodes so spatially close nodes
    // are also close in memory, providing uniform cache performance at all depths.
    // For simplicity in the initial implementation, we use a BFS-order layout
    // which provides good cache behavior for breadth-first traversal.
    // A full vEB layout would recursively split the tree at the median depth.

    if (node_count_ <= 2) return;

    // Allocate temporary buffer for rearranged nodes
    BVHNode* new_nodes = static_cast<BVHNode*>(
        allocator.allocate(node_count_ * sizeof(BVHNode)));
    if (!new_nodes) return;

    // BFS-order layout (approximation of vEB for initial implementation)
    std::vector<uint32_t> bfs_order;
    bfs_order.reserve(node_count_);

    std::vector<uint32_t> queue;
    queue.push_back(0);

    while (!queue.empty()) {
        std::vector<uint32_t> next_level;
        for (uint32_t idx : queue) {
            bfs_order.push_back(idx);
            if (!nodes_[idx].is_leaf()) {
                uint32_t left = nodes_[idx].child_or_object_index;
                if (left < node_count_) next_level.push_back(left);
                if (left + 1 < node_count_) next_level.push_back(left + 1);
            }
        }
        queue = std::move(next_level);
    }

    // Create index remapping
    std::vector<uint32_t> old_to_new(node_count_, 0);
    for (uint32_t new_idx = 0; new_idx < bfs_order.size(); ++new_idx) {
        old_to_new[bfs_order[new_idx]] = new_idx;
    }

    // Copy nodes in new order, updating child indices
    for (uint32_t new_idx = 0; new_idx < bfs_order.size(); ++new_idx) {
        uint32_t old_idx = bfs_order[new_idx];
        new_nodes[new_idx] = nodes_[old_idx];
        if (!new_nodes[new_idx].is_leaf()) {
            uint32_t old_left = nodes_[old_idx].child_or_object_index;
            new_nodes[new_idx].child_or_object_index = old_to_new[old_left];
        }
    }

    // Swap to new layout
    nodes_ = new_nodes;
}

} // namespace pictor
