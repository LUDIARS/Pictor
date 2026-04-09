#pragma once

#include "pictor/core/types.h"
#include "pictor/batch/radix_sort.h"
#include "pictor/scene/scene_registry.h"
#include "pictor/memory/frame_allocator.h"
#include <vector>

namespace pictor {

/// Custom batching policy interface (§12.2)
class IBatchPolicy {
public:
    virtual ~IBatchPolicy() = default;

    /// Determine if two consecutive sorted objects should be in the same batch
    virtual bool should_merge(uint64_t key_a, uint64_t key_b) const = 0;
};

/// Batch builder: generates RenderBatches from sorted SoA streams (§6.1, §2.2).
/// Uses index indirection — actual data never moves.
class BatchBuilder {
public:
    explicit BatchBuilder(SceneRegistry& registry);
    ~BatchBuilder() = default;

    /// Build batches for all pools. Uses frame allocator for temporaries.
    void build(FrameAllocator& allocator);

    /// Invalidate batches (§6.3)
    void invalidate_all();
    void invalidate_pool(PoolType type);

    /// Set custom batch policy (§12.2)
    void set_batch_policy(IBatchPolicy* policy) { policy_ = policy; }

    /// Get generated batches
    const std::vector<RenderBatch>& batches() const { return batches_; }

    /// Get sorted index array (for indirect access to SoA data)
    const uint32_t* sorted_indices() const { return sorted_indices_; }
    size_t sorted_index_count() const { return sorted_index_count_; }

    /// Statistics
    struct Stats {
        uint32_t total_batches     = 0;
        uint32_t static_batches    = 0;
        uint32_t dynamic_batches   = 0;
        uint32_t gpu_driven_batches = 0;
        uint32_t total_objects     = 0;
    };

    Stats get_stats() const;

private:
    /// Build batches for Static Pool (§6.1 - Multi Draw Indirect)
    void build_static(FrameAllocator& allocator);

    /// Build batches for Dynamic Pool (§6.1 - Instanced Draw)
    void build_dynamic(FrameAllocator& allocator);

    /// Build batches for GPU Driven Pool (§6.1 - no CPU batching)
    void build_gpu_driven();

    /// Generate sort keys and sort
    void sort_pool(ObjectPool& pool, FrameAllocator& allocator,
                   SortPair*& out_pairs, size_t& out_count);

    /// Create batches from sorted pairs
    void create_batches_from_sorted(const SortPair* pairs, size_t count,
                                    const ObjectPool& pool);

    SceneRegistry&         registry_;
    std::vector<RenderBatch> batches_;
    IBatchPolicy*          policy_   = nullptr;
    uint32_t*              sorted_indices_ = nullptr;
    size_t                 sorted_index_count_ = 0;
    bool                   dirty_[3] = {true, true, true};
};

} // namespace pictor
