#include "pictor/batch/batch_builder.h"

namespace pictor {

BatchBuilder::BatchBuilder(SceneRegistry& registry)
    : registry_(registry)
{
}

void BatchBuilder::build(FrameAllocator& allocator) {
    batches_.clear();

    // Build batches for each pool type (§6.1).
    // Static batches are only re-sorted when the static pool changes, but the
    // cached result is merged into every frame so static geometry keeps drawing.
    if (dirty_[static_cast<int>(PoolType::STATIC)]) {
        build_static(allocator);
    }
    batches_.insert(batches_.end(), static_batches_.begin(), static_batches_.end());

    build_dynamic(allocator);  // Always rebuild dynamic batches
    build_gpu_driven();

    dirty_[static_cast<int>(PoolType::STATIC)] = false;
    dirty_[static_cast<int>(PoolType::DYNAMIC)] = false;
    dirty_[static_cast<int>(PoolType::GPU_DRIVEN)] = false;
}

void BatchBuilder::invalidate_all() {
    dirty_[0] = dirty_[1] = dirty_[2] = true;
}

void BatchBuilder::invalidate_pool(PoolType type) {
    dirty_[static_cast<int>(type)] = true;
}

void BatchBuilder::sort_pool(ObjectPool& pool, FrameAllocator& allocator,
                              SortPair*& out_pairs, size_t& out_count) {
    uint32_t count = pool.count();
    if (count == 0) {
        out_pairs = nullptr;
        out_count = 0;
        return;
    }

    // Allocate key-index pairs from frame allocator (§6.2)
    auto* pairs = allocator.allocate_array<SortPair>(count);
    if (!pairs) {
        out_pairs = nullptr;
        out_count = 0;
        return;
    }

    // Build sort keys (§6.2)
    const uint64_t* shader_keys = pool.shader_keys().data();
    const uint32_t* material_keys = pool.material_keys().data();
    const uint8_t* visibility = pool.visibility_flags().data();

    for (uint32_t i = 0; i < count; ++i) {
        if (visibility[i] == 0) {
            // Culled objects get max sort key (pushed to end)
            pairs[i].key = UINT64_MAX;
        } else {
            // Build composite sort key
            uint8_t transparency = (pool.flags().data()[i] & ObjectFlags::TRANSPARENT) ? 1 : 0;
            pairs[i].key = RadixSort::build_sort_key(
                0, // render pass id (set by pass scheduler)
                transparency,
                static_cast<uint16_t>(shader_keys[i] >> 48),
                static_cast<uint16_t>(material_keys[i]),
                0  // depth (computed per-camera)
            );
        }
        pairs[i].index = i;
        pairs[i].padding = 0;
    }

    // Radix sort the key-index pairs (§6.2)
    RadixSort::sort(pairs, count, allocator);

    out_pairs = pairs;
    out_count = count;
}

void BatchBuilder::create_batches_from_sorted(const SortPair* pairs, size_t count,
                                               const ObjectPool& pool,
                                               std::vector<RenderBatch>& out) {
    if (count == 0) return;

    // Skip culled objects (sort key == UINT64_MAX)
    size_t visible_count = count;
    while (visible_count > 0 && pairs[visible_count - 1].key == UINT64_MAX) {
        --visible_count;
    }

    if (visible_count == 0) return;

    // Group consecutive objects with matching shaderKey + materialKey into
    // batches (§6.1)。 透過区分は追加の強制境界 — 1 batch = 1 カテゴリでない
    // と `CompiledPass::filter_mask` で opaque/transparent pass へ振り分け
    // られない (RenderBatchFilter)。
    RenderBatch current_batch;
    current_batch.sortKey = pairs[0].key;
    current_batch.startIndex = 0;
    current_batch.count = 1;
    current_batch.shaderKey = pool.shader_keys()[pairs[0].index];
    current_batch.materialKey = pool.material_keys()[pairs[0].index];
    current_batch.mesh = pool.mesh_handles().data()[pairs[0].index];
    current_batch.transparency = RadixSort::transparency_of(pairs[0].key);

    for (size_t i = 1; i < visible_count; ++i) {
        uint32_t idx = pairs[i].index;
        uint64_t sk = pool.shader_keys()[idx];
        uint32_t mk = pool.material_keys()[idx];

        const uint8_t transparency = RadixSort::transparency_of(pairs[i].key);
        bool should_merge = (sk == current_batch.shaderKey &&
                            mk == current_batch.materialKey &&
                            transparency == current_batch.transparency);

        if (policy_) {
            should_merge = transparency == current_batch.transparency &&
                           policy_->should_merge(pairs[i - 1].key, pairs[i].key);
        }

        if (should_merge) {
            current_batch.count++;
        } else {
            out.push_back(current_batch);
            current_batch.sortKey = pairs[i].key;
            current_batch.startIndex = static_cast<uint32_t>(i);
            current_batch.count = 1;
            current_batch.shaderKey = sk;
            current_batch.materialKey = mk;
            current_batch.mesh = pool.mesh_handles().data()[idx];
            current_batch.transparency = transparency;
        }
    }

    out.push_back(current_batch);
}

void BatchBuilder::build_static(FrameAllocator& allocator) {
    // §6.1: Static Pool — Multi Draw Indirect with index indirection.
    // Rebuild the cache from scratch; build() merges it into batches_ each frame.
    static_batches_.clear();
    ObjectPool& pool = registry_.static_pool();
    SortPair* pairs;
    size_t count;
    sort_pool(pool, allocator, pairs, count);
    create_batches_from_sorted(pairs, count, pool, static_batches_);
}

void BatchBuilder::build_dynamic(FrameAllocator& allocator) {
    // §6.1: Dynamic Pool — Instanced Draw
    // sorted_indices_ points into frame-allocator memory; drop the previous
    // frame's pointer up front so an empty/alloc-failed frame never exposes
    // a reset region through sorted_indices().
    sorted_indices_ = nullptr;
    sorted_index_count_ = 0;
    ObjectPool& pool = registry_.dynamic_pool();
    SortPair* pairs;
    size_t count;
    sort_pool(pool, allocator, pairs, count);
    create_batches_from_sorted(pairs, count, pool, batches_);

    // Store sorted indices for indirect data access
    if (count > 0 && pairs) {
        sorted_indices_ = allocator.allocate_array<uint32_t>(count);
        if (sorted_indices_) {
            for (size_t i = 0; i < count; ++i) {
                sorted_indices_[i] = pairs[i].index;
            }
            sorted_index_count_ = count;
        }
    }
}

void BatchBuilder::build_gpu_driven() {
    // §6.1: GPU Driven Pool — no CPU-side batching
    // Compute Shader handles culling, LOD, and draw command generation
    ObjectPool& pool = registry_.gpu_driven_pool();
    if (pool.empty()) return;

    // Create a single "batch" representing the GPU Driven pool
    RenderBatch batch;
    batch.sortKey = 0;
    batch.startIndex = 0;
    batch.count = pool.count();
    batch.shaderKey = 0; // GPU determines shader
    batch.materialKey = 0;
    batch.transparency = 0;
    batches_.push_back(batch);
}

BatchBuilder::Stats BatchBuilder::get_stats() const {
    Stats stats;
    stats.total_batches = static_cast<uint32_t>(batches_.size());
    stats.total_objects = registry_.total_object_count();
    return stats;
}

} // namespace pictor
