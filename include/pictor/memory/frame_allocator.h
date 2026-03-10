#pragma once

#include <cstddef>
#include <cstdint>
#include <atomic>
#include <vector>
#include <cassert>

namespace pictor {

/// Linear bump allocator that resets each frame (§4.3).
/// O(1) allocation, lock-free via atomic bump pointer.
/// No individual deallocation — pointer resets at frame end.
class FrameAllocator {
public:
    explicit FrameAllocator(size_t capacity = 16 * 1024 * 1024); // default 16MB
    ~FrameAllocator();

    FrameAllocator(const FrameAllocator&) = delete;
    FrameAllocator& operator=(const FrameAllocator&) = delete;
    FrameAllocator(FrameAllocator&& other) noexcept;
    FrameAllocator& operator=(FrameAllocator&& other) noexcept;

    /// Allocate `size` bytes with given alignment. Returns nullptr if out of space.
    void* allocate(size_t size, size_t alignment = 16);

    /// Typed allocation helper
    template<typename T>
    T* allocate_array(size_t count) {
        return static_cast<T*>(allocate(sizeof(T) * count, alignof(T)));
    }

    /// Reset pointer to beginning (called at frame start)
    void reset();

    /// Current usage in bytes
    size_t used() const { return offset_.load(std::memory_order_relaxed); }

    /// Peak usage since last reset
    size_t peak() const { return peak_; }

    /// Total capacity
    size_t capacity() const { return capacity_; }

private:
    uint8_t*          buffer_   = nullptr;
    size_t            capacity_ = 0;
    std::atomic<size_t> offset_{0};
    size_t            peak_     = 0;
    bool              owns_memory_ = true;
};

/// Double/triple buffered frame allocators for flight management (§4.3).
class FlightFrameAllocator {
public:
    explicit FlightFrameAllocator(size_t per_frame_capacity, uint32_t flight_count = 3);

    /// Get the allocator for the current frame
    FrameAllocator& current() { return allocators_[current_index_]; }

    /// Advance to next frame (resets the allocator that will be reused)
    void advance_frame();

    uint32_t flight_count() const { return static_cast<uint32_t>(allocators_.size()); }
    uint32_t current_index() const { return current_index_; }

    /// Get stats across all flights
    size_t total_capacity() const;

private:
    std::vector<FrameAllocator> allocators_;
    uint32_t current_index_ = 0;
};

} // namespace pictor
