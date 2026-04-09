#include "pictor/update/update_scheduler.h"
#include <cstring>

#ifdef __AVX2__
#include <immintrin.h>
#endif

namespace pictor {

UpdateScheduler::UpdateScheduler(SceneRegistry& registry)
    : UpdateScheduler(registry, UpdateConfig{})
{}

UpdateScheduler::UpdateScheduler(SceneRegistry& registry, const UpdateConfig& config)
    : registry_(registry)
    , config_(config)
{
    uint32_t threads = config.worker_threads;
    if (threads == 0) {
        threads = std::max(1u,
            static_cast<uint32_t>(std::thread::hardware_concurrency()) - 1);
    }
    default_dispatcher_ = std::make_unique<ThreadPoolDispatcher>(threads);
    dispatcher_ = default_dispatcher_.get();
}

UpdateScheduler::~UpdateScheduler() = default;

void UpdateScheduler::set_job_dispatcher(IJobDispatcher* dispatcher) {
    dispatcher_ = dispatcher ? dispatcher : default_dispatcher_.get();
}

UpdateStrategy UpdateScheduler::select_strategy(PoolType type, uint32_t object_count) const {
    // §5.5: Automatic strategy selection
    switch (type) {
        case PoolType::STATIC:
            return UpdateStrategy::NONE;

        case PoolType::GPU_DRIVEN:
            if (registry_.compute_update_shader() != INVALID_MESH) {
                return UpdateStrategy::GPU_COMPUTE;
            }
            // Fall through to CPU update if no compute shader
            [[fallthrough]];

        case PoolType::DYNAMIC:
            if (object_count > config_.nt_store_threshold && config_.nt_store_enabled) {
                return UpdateStrategy::CPU_PARALLEL_NT;
            }
            return UpdateStrategy::CPU_PARALLEL;
    }
    return UpdateStrategy::NONE;
}

void UpdateScheduler::update(float delta_time) {
    total_time_ += delta_time;

    // Update each pool with the appropriate strategy (§5.5)
    for (int i = 0; i < 3; ++i) {
        auto pool_type = static_cast<PoolType>(i);
        ObjectPool& pool = registry_.pool(pool_type);

        if (pool.empty()) {
            strategies_[i] = UpdateStrategy::NONE;
            continue;
        }

        strategies_[i] = select_strategy(pool_type, pool.count());

        switch (strategies_[i]) {
            case UpdateStrategy::NONE:
                break;

            case UpdateStrategy::CPU_PARALLEL:
                copy_prev_transforms(pool);
                update_cpu_parallel(pool, delta_time);
                break;

            case UpdateStrategy::CPU_PARALLEL_NT:
                copy_prev_transforms(pool);
                update_cpu_parallel_nt(pool, delta_time);
                break;

            case UpdateStrategy::GPU_COMPUTE:
                prepare_compute_update(delta_time);
                break;
        }
    }
}

void UpdateScheduler::update_cpu_parallel(ObjectPool& pool, float delta_time) {
    if (!callback_) return;

    uint32_t count = pool.count();
    float4x4* transforms = pool.transforms().data();
    AABB* bounds = pool.bounds().data();

    // §5.2: Dispatch parallel jobs with SoA range splitting
    dispatcher_->dispatch(count, config_.chunk_size,
        [this, transforms, bounds, delta_time](uint32_t start, uint32_t end) {
            callback_->update(transforms, bounds, start, end - start, delta_time);
        });

    dispatcher_->wait_all();
}

void UpdateScheduler::update_cpu_parallel_nt(ObjectPool& pool, float delta_time) {
    if (!callback_) return;

    uint32_t count = pool.count();
    float4x4* transforms = pool.transforms().data();
    AABB* bounds = pool.bounds().data();

    // §5.3: Level 2 — NT Store
    // First update via callback, then write with non-temporal stores
    dispatcher_->dispatch(count, config_.chunk_size,
        [this, transforms, bounds, delta_time](uint32_t start, uint32_t end) {
            // Update to temporary first, then NT store
            callback_->update(transforms, bounds, start, end - start, delta_time);

#ifdef __AVX2__
            // Use NT stores for transforms (64B = 1 cache line) (§5.3)
            // float4x4 is exactly 64 bytes = 1 cache line, optimal for NT store
            for (uint32_t i = start; i < end; ++i) {
                const float* src = reinterpret_cast<const float*>(&transforms[i]);
                float* dst = reinterpret_cast<float*>(&transforms[i]);
                // Stream 64 bytes (2 x 256-bit AVX stores)
                __m256 v0 = _mm256_load_ps(src);
                __m256 v1 = _mm256_load_ps(src + 8);
                _mm256_stream_ps(dst, v0);
                _mm256_stream_ps(dst + 8, v1);
            }
            _mm_sfence(); // Ensure NT stores are visible
#endif
        });

    dispatcher_->wait_all();
}

void UpdateScheduler::prepare_compute_update(float delta_time) {
    // §5.4: Prepare uniform data for GPU Compute Update
    compute_params_.delta_time = delta_time;
    compute_params_.total_time = total_time_;
    compute_params_.frame_number = registry_.gpu_driven_pool().count();
    // GPU dispatch happens in GPUDrivenPipeline::execute()
}

void UpdateScheduler::copy_prev_transforms(ObjectPool& pool) {
    // Copy current transforms to prevTransforms for motion vectors (§3.2)
    uint32_t count = pool.count();
    if (count == 0) return;

    std::memcpy(pool.prev_transforms().data(),
                pool.transforms().data(),
                count * sizeof(float4x4));
}

} // namespace pictor
