#pragma once

#include <chrono>
#include <cstdint>

namespace pictor {

/// High-resolution CPU timer (§13.3).
/// Uses std::chrono::high_resolution_clock (or platform-specific QPC/rdtsc).
class CpuTimer {
public:
    void start() {
        start_ = std::chrono::high_resolution_clock::now();
    }

    void stop() {
        end_ = std::chrono::high_resolution_clock::now();
    }

    /// Elapsed time in milliseconds
    double elapsed_ms() const {
        auto dur = std::chrono::duration_cast<std::chrono::nanoseconds>(end_ - start_);
        return dur.count() / 1e6;
    }

    /// Elapsed time in microseconds
    double elapsed_us() const {
        auto dur = std::chrono::duration_cast<std::chrono::nanoseconds>(end_ - start_);
        return dur.count() / 1e3;
    }

private:
    std::chrono::high_resolution_clock::time_point start_;
    std::chrono::high_resolution_clock::time_point end_;
};

/// Scoped CPU timer that stops on destruction
class ScopedCpuTimer {
public:
    explicit ScopedCpuTimer(double& out_ms) : out_ms_(out_ms) {
        timer_.start();
    }

    ~ScopedCpuTimer() {
        timer_.stop();
        out_ms_ = timer_.elapsed_ms();
    }

private:
    CpuTimer timer_;
    double&  out_ms_;
};

} // namespace pictor
