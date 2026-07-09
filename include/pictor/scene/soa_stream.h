#pragma once

#include "pictor/core/types.h"
#include "pictor/memory/pool_allocator.h"
#include <cstring>
#include <algorithm>
#include <utility>

namespace pictor {

/// Type-safe SoA stream — a contiguous array of a single attribute (§3.1, §3.2).
/// All render object data is stored as parallel SoA streams indexed by the same slot.
template<typename T>
class SoAStream {
public:
    SoAStream() = default;

    explicit SoAStream(PoolAllocator* allocator, size_t initial_capacity = 1024)
        : allocator_(allocator)
    {
        reserve(initial_capacity);
    }

    ~SoAStream() = default; // allocator owns the memory

    SoAStream(const SoAStream&) = delete;
    SoAStream& operator=(const SoAStream&) = delete;
    // Explicit moves: null out the source so two streams never alias data_
    // (the allocator owns the memory, but a stale data_ invites double writes).
    SoAStream(SoAStream&& other) noexcept
        : data_(std::exchange(other.data_, nullptr)),
          size_(std::exchange(other.size_, 0)),
          capacity_(std::exchange(other.capacity_, 0)),
          allocator_(std::exchange(other.allocator_, nullptr)) {}

    SoAStream& operator=(SoAStream&& other) noexcept {
        if (this != &other) {
            data_      = std::exchange(other.data_, nullptr);
            size_      = std::exchange(other.size_, 0);
            capacity_  = std::exchange(other.capacity_, 0);
            allocator_ = std::exchange(other.allocator_, nullptr);
        }
        return *this;
    }

    void init(PoolAllocator* allocator, size_t initial_capacity = 1024) {
        allocator_ = allocator;
        reserve(initial_capacity);
    }

    void reserve(size_t new_capacity) {
        if (new_capacity <= capacity_) return;
        // Round up to cache-line aligned count
        size_t aligned_cap = ((new_capacity * sizeof(T) + 63) / 64) * 64 / sizeof(T);
        if (aligned_cap < new_capacity) aligned_cap = new_capacity;

        T* new_data = static_cast<T*>(
            allocator_->reallocate_array(data_, sizeof(T), size_, aligned_cap));
        data_ = new_data;
        capacity_ = aligned_cap;
    }

    /// Add element at end, returns index
    uint32_t push_back(const T& value) {
        if (size_ >= capacity_) {
            reserve(capacity_ == 0 ? 1024 : capacity_ * 2);
        }
        data_[size_] = value;
        return static_cast<uint32_t>(size_++);
    }

    /// Swap-and-pop removal (§4.1). Out-of-range index is a no-op
    /// (size_ - 1 would wrap to SIZE_MAX when empty).
    void swap_and_pop(uint32_t index) {
        if (index >= size_) return;
        if (index < size_ - 1) {
            data_[index] = data_[size_ - 1];
        }
        --size_;
    }

    T& operator[](size_t i) { return data_[i]; }
    const T& operator[](size_t i) const { return data_[i]; }

    T*       data()       { return data_; }
    const T* data() const { return data_; }
    size_t   size() const { return size_; }
    size_t   capacity() const { return capacity_; }
    bool     empty() const { return size_ == 0; }

    void clear() { size_ = 0; }

    /// Direct size manipulation (for parallel stream operations)
    void set_size(size_t s) { size_ = s; }

private:
    T*             data_      = nullptr;
    size_t         size_      = 0;
    size_t         capacity_  = 0;
    PoolAllocator* allocator_ = nullptr;
};

} // namespace pictor
