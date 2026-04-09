#include "pictor/update/job_dispatcher.h"
#include <algorithm>

namespace pictor {

ThreadPoolDispatcher::ThreadPoolDispatcher(uint32_t thread_count) {
    if (thread_count == 0) {
        // Auto-detect: CPU logical cores - 1 (§5.2)
        thread_count = std::max(1u,
            static_cast<uint32_t>(std::thread::hardware_concurrency()) - 1);
    }
    thread_count_ = thread_count;

    threads_.reserve(thread_count);
    for (uint32_t i = 0; i < thread_count; ++i) {
        threads_.emplace_back(&ThreadPoolDispatcher::worker_thread, this);
    }
}

ThreadPoolDispatcher::~ThreadPoolDispatcher() {
    shutdown_.store(true, std::memory_order_release);
    cv_task_.notify_all();
    for (auto& t : threads_) {
        if (t.joinable()) t.join();
    }
}

void ThreadPoolDispatcher::dispatch(uint32_t count, uint32_t chunk_size, JobFunction fn) {
    if (count == 0) return;

    // Align chunk boundaries to 64B cache lines (§5.2)
    // For float4x4 (64B each), each element is exactly one cache line
    uint32_t aligned_chunk = chunk_size;

    uint32_t job_count = (count + aligned_chunk - 1) / aligned_chunk;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (uint32_t i = 0; i < job_count; ++i) {
            uint32_t start = i * aligned_chunk;
            uint32_t end = std::min(start + aligned_chunk, count);
            task_queue_.push({fn, start, end});
        }
        pending_tasks_.store(job_count, std::memory_order_release);
    }
    cv_task_.notify_all();
}

void ThreadPoolDispatcher::wait_all() {
    std::unique_lock<std::mutex> lock(mutex_);
    cv_done_.wait(lock, [this] {
        return pending_tasks_.load(std::memory_order_acquire) == 0;
    });
}

void ThreadPoolDispatcher::worker_thread() {
    while (true) {
        Task task;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_task_.wait(lock, [this] {
                return !task_queue_.empty() || shutdown_.load(std::memory_order_acquire);
            });

            if (shutdown_.load(std::memory_order_acquire) && task_queue_.empty()) {
                return;
            }

            task = task_queue_.front();
            task_queue_.pop();
        }

        // Execute the job
        task.fn(task.start, task.end);

        if (pending_tasks_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            cv_done_.notify_all();
        }
    }
}

} // namespace pictor
