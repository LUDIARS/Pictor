#pragma once

#include "pictor/update/job_dispatcher.h"

namespace pictor {

/// IJobDispatcher that runs every job inline on the calling thread.
///
/// The default on Emscripten builds without pthreads (no worker threads can
/// be started there), and usable anywhere a deterministic, single-threaded
/// update is wanted (headless tests, debugging). dispatch() returns only once
/// the whole range is processed, so wait_all() has nothing to wait for.
class SingleThreadDispatcher final : public IJobDispatcher {
public:
    void dispatch(uint32_t count, uint32_t chunk_size, JobFunction fn) override;
    void wait_all() override {}
    uint32_t worker_count() const override { return 1; }
};

} // namespace pictor
