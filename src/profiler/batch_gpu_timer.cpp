#include "pictor/profiler/batch_gpu_timer.h"
#include "pictor/profiler/gpu_timer.h"

#include <cstdio>

namespace pictor {

BatchGpuTimer::BatchGpuTimer(GpuTimerManager& gpu) : gpu_(gpu) {}

const char* BatchGpuTimer::region_name_(uint32_t index) {
    // 必要数まで安定ストレージを伸長する。GpuTimerManager は const char* を
    // strcmp 照合するので、寿命さえ保てばポインタ自体は毎フレーム同じでよい。
    if (index >= names_.size()) {
        const size_t old = names_.size();
        names_.resize(index + 1);
        for (size_t i = old; i < names_.size(); ++i) {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "batch_%zu", i);
            names_[i] = buf;
        }
    }
    return names_[index].c_str();
}

void BatchGpuTimer::begin_frame(const std::vector<RenderBatch>& batches) {
    meta_.clear();
    meta_.reserve(batches.size());
    for (uint32_t i = 0; i < batches.size(); ++i) {
        const RenderBatch& b = batches[i];
        region_name_(i); // 名前ストレージを確保しておく

        BatchTimingInfo info;
        info.batch_index  = i;
        info.mesh         = b.mesh;
        info.object_count = b.count;
        // pool は shaderKey の CUSTOM ビット等からは判別できないため、
        // 呼び出し側が batch を pool 別に渡す運用。ここでは DYNAMIC 既定。
        info.pool         = PoolType::DYNAMIC;
        meta_.push_back(info);
    }
}

#ifdef PICTOR_HAS_VULKAN
void BatchGpuTimer::begin_batch(uint32_t batch_index, VkCommandBuffer cmd) {
    gpu_.begin_region(region_name_(batch_index), cmd);
}

void BatchGpuTimer::end_batch(uint32_t batch_index, VkCommandBuffer cmd) {
    gpu_.end_region(region_name_(batch_index), cmd);
}
#endif

void BatchGpuTimer::begin_batch(uint32_t batch_index) {
    gpu_.begin_region(region_name_(batch_index));
}

void BatchGpuTimer::end_batch(uint32_t batch_index) {
    gpu_.end_region(region_name_(batch_index));
}

std::vector<BatchTimingInfo> BatchGpuTimer::timings() const {
    std::vector<BatchTimingInfo> out = meta_;
    for (auto& info : out) {
        // region 名は const ストレージ。const メソッドだが names_ は確保済み。
        const std::string& name = names_[info.batch_index];
        const double ms = gpu_.get_region_ms(name.c_str());
        info.gpu_ms = ms;
        info.measured = (ms > 0.0);
        info.per_object_us = (info.object_count > 0)
            ? (ms * 1000.0) / static_cast<double>(info.object_count)
            : 0.0;
    }
    return out;
}

} // namespace pictor
