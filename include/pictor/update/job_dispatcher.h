#pragma once

#include <cstdint>
#include <functional>
#include <vector>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <queue>

namespace pictor {

/// Job descriptor for parallel SoA range updates (§5.2)
struct UpdateJob {
    uint32_t start_index;
    uint32_t end_index;
};

/// Interface for external job system replacement (§5.2 Design Note)
class IJobDispatcher {
public:
    virtual ~IJobDispatcher() = default;

    using JobFunction = std::function<void(uint32_t start, uint32_t end)>;

    /// Dispatch parallel jobs across a range [0, count)
    /// with given chunk_size per job
    virtual void dispatch(uint32_t count, uint32_t chunk_size, JobFunction fn) = 0;

    /// Wait for all dispatched jobs to complete
    virtual void wait_all() = 0;

    virtual uint32_t worker_count() const = 0;
};

/// Built-in thread pool job dispatcher (§5.2)
class ThreadPoolDispatcher : public IJobDispatcher {
public:
    explicit ThreadPoolDispatcher(uint32_t thread_count = 0); // 0 = auto-detect
    ~ThreadPoolDispatcher() override;

    void dispatch(uint32_t count, uint32_t chunk_size, JobFunction fn) override;
    void wait_all() override;
    uint32_t worker_count() const override { return thread_count_; }

private:
    void worker_thread();

    struct Task {
        JobFunction fn;
        uint32_t start;
        uint32_t end;
    };

    std::vector<std::thread> threads_;
    std::queue<Task>         task_queue_;
    std::mutex               mutex_;
    std::condition_variable  cv_task_;
    std::condition_variable  cv_done_;
    std::atomic<uint32_t>    pending_tasks_{0};
    std::atomic<bool>        shutdown_{false};
    uint32_t                 thread_count_ = 0;
};

} // namespace pictor
