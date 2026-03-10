#include "pictor/memory/frame_allocator.h"
#include <cstdlib>
#include <algorithm>
#include <new>

#ifdef PICTOR_LARGE_PAGES
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif
#endif

namespace pictor {

// ============================================================
// FrameAllocator
// ============================================================

FrameAllocator::FrameAllocator(size_t capacity)
    : capacity_(capacity)
{
#ifdef PICTOR_LARGE_PAGES
    // Attempt 2MB large page allocation for TLB efficiency (§4.3, §5.3)
#ifdef _WIN32
    buffer_ = static_cast<uint8_t*>(
        VirtualAlloc(nullptr, capacity, MEM_COMMIT | MEM_RESERVE | MEM_LARGE_PAGES, PAGE_READWRITE));
    if (!buffer_) {
        buffer_ = static_cast<uint8_t*>(
            VirtualAlloc(nullptr, capacity, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    }
    owns_memory_ = true;
#else
    buffer_ = static_cast<uint8_t*>(
        mmap(nullptr, capacity, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0));
    if (buffer_ == MAP_FAILED) {
        buffer_ = static_cast<uint8_t*>(
            mmap(nullptr, capacity, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    }
    owns_memory_ = true;
#endif
#else
    // Standard aligned allocation (64-byte alignment for cache lines)
    buffer_ = static_cast<uint8_t*>(std::aligned_alloc(64, capacity));
    owns_memory_ = true;
#endif

    if (!buffer_) {
        throw std::bad_alloc();
    }
}

FrameAllocator::~FrameAllocator() {
    if (buffer_ && owns_memory_) {
#ifdef PICTOR_LARGE_PAGES
#ifdef _WIN32
        VirtualFree(buffer_, 0, MEM_RELEASE);
#else
        munmap(buffer_, capacity_);
#endif
#else
        std::free(buffer_);
#endif
    }
}

FrameAllocator::FrameAllocator(FrameAllocator&& other) noexcept
    : buffer_(other.buffer_)
    , capacity_(other.capacity_)
    , offset_(other.offset_.load(std::memory_order_relaxed))
    , peak_(other.peak_)
    , owns_memory_(other.owns_memory_)
{
    other.buffer_ = nullptr;
    other.capacity_ = 0;
    other.offset_.store(0, std::memory_order_relaxed);
    other.owns_memory_ = false;
}

FrameAllocator& FrameAllocator::operator=(FrameAllocator&& other) noexcept {
    if (this != &other) {
        if (buffer_ && owns_memory_) {
            std::free(buffer_);
        }
        buffer_ = other.buffer_;
        capacity_ = other.capacity_;
        offset_.store(other.offset_.load(std::memory_order_relaxed), std::memory_order_relaxed);
        peak_ = other.peak_;
        owns_memory_ = other.owns_memory_;
        other.buffer_ = nullptr;
        other.capacity_ = 0;
        other.offset_.store(0, std::memory_order_relaxed);
        other.owns_memory_ = false;
    }
    return *this;
}

void* FrameAllocator::allocate(size_t size, size_t alignment) {
    // Atomic bump allocation — lock-free, O(1) (§4.3)
    size_t current = offset_.load(std::memory_order_relaxed);
    size_t aligned;

    do {
        aligned = (current + alignment - 1) & ~(alignment - 1);
        size_t new_offset = aligned + size;

        if (new_offset > capacity_) {
            return nullptr; // out of space
        }

        if (offset_.compare_exchange_weak(current, new_offset,
                                          std::memory_order_relaxed)) {
            // Update peak (not atomic — slight race is acceptable for stats)
            if (new_offset > peak_) {
                peak_ = new_offset;
            }
            return buffer_ + aligned;
        }
    } while (true);
}

void FrameAllocator::reset() {
    offset_.store(0, std::memory_order_relaxed);
}

// ============================================================
// FlightFrameAllocator
// ============================================================

FlightFrameAllocator::FlightFrameAllocator(size_t per_frame_capacity, uint32_t flight_count) {
    allocators_.reserve(flight_count);
    for (uint32_t i = 0; i < flight_count; ++i) {
        allocators_.emplace_back(per_frame_capacity);
    }
}

void FlightFrameAllocator::advance_frame() {
    current_index_ = (current_index_ + 1) % static_cast<uint32_t>(allocators_.size());
    // Reset the allocator for the new frame (§4.3: Frame N-2 reuse)
    allocators_[current_index_].reset();
}

size_t FlightFrameAllocator::total_capacity() const {
    size_t total = 0;
    for (const auto& a : allocators_) {
        total += a.capacity();
    }
    return total;
}

} // namespace pictor
