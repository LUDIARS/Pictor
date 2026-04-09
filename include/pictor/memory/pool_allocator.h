#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace pictor {

/// Chunk-based pool allocator for SoA stream backing storage (§11.1).
/// Allocates in fixed-size chunks and grows as needed.
/// Supports 64-byte cache-line aligned allocations.
class PoolAllocator {
public:
    static constexpr size_t DEFAULT_CHUNK_SIZE = 64 * 1024; // 64KB chunks
    static constexpr size_t CACHE_LINE = 64;

    explicit PoolAllocator(size_t chunk_size = DEFAULT_CHUNK_SIZE);
    ~PoolAllocator();

    PoolAllocator(const PoolAllocator&) = delete;
    PoolAllocator& operator=(const PoolAllocator&) = delete;
    PoolAllocator(PoolAllocator&&) noexcept;
    PoolAllocator& operator=(PoolAllocator&&) noexcept;

    /// Allocate `size` bytes with cache-line alignment
    void* allocate(size_t size);

    /// Allocate a contiguous array for `count` elements of `element_size` bytes
    void* allocate_array(size_t element_size, size_t count);

    /// Reallocate an array, preserving existing data up to old_count
    void* reallocate_array(void* old_ptr, size_t element_size,
                           size_t old_count, size_t new_count);

    /// Free all memory
    void clear();

    size_t total_allocated() const { return total_allocated_; }
    size_t chunk_count() const { return chunks_.size(); }

private:
    struct Chunk {
        uint8_t* data     = nullptr;
        size_t   capacity = 0;
        size_t   used     = 0;
    };

    void add_chunk(size_t min_size);

    std::vector<Chunk> chunks_;
    size_t chunk_size_;
    size_t total_allocated_ = 0;
};

} // namespace pictor
