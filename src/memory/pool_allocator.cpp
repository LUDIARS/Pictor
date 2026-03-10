#include "pictor/memory/pool_allocator.h"
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <new>

namespace pictor {

PoolAllocator::PoolAllocator(size_t chunk_size)
    : chunk_size_(chunk_size)
{
}

PoolAllocator::~PoolAllocator() {
    clear();
}

PoolAllocator::PoolAllocator(PoolAllocator&& other) noexcept
    : chunks_(std::move(other.chunks_))
    , chunk_size_(other.chunk_size_)
    , total_allocated_(other.total_allocated_)
{
    other.total_allocated_ = 0;
}

PoolAllocator& PoolAllocator::operator=(PoolAllocator&& other) noexcept {
    if (this != &other) {
        clear();
        chunks_ = std::move(other.chunks_);
        chunk_size_ = other.chunk_size_;
        total_allocated_ = other.total_allocated_;
        other.total_allocated_ = 0;
    }
    return *this;
}

void* PoolAllocator::allocate(size_t size) {
    // Align to cache line (64 bytes)
    size = (size + CACHE_LINE - 1) & ~(CACHE_LINE - 1);

    // Try to fit in existing chunks
    for (auto& chunk : chunks_) {
        size_t aligned_used = (chunk.used + CACHE_LINE - 1) & ~(CACHE_LINE - 1);
        if (aligned_used + size <= chunk.capacity) {
            void* ptr = chunk.data + aligned_used;
            chunk.used = aligned_used + size;
            total_allocated_ += size;
            return ptr;
        }
    }

    // Need a new chunk
    add_chunk(size);
    auto& chunk = chunks_.back();
    void* ptr = chunk.data;
    chunk.used = size;
    total_allocated_ += size;
    return ptr;
}

void* PoolAllocator::allocate_array(size_t element_size, size_t count) {
    return allocate(element_size * count);
}

void* PoolAllocator::reallocate_array(void* old_ptr, size_t element_size,
                                       size_t old_count, size_t new_count) {
    size_t old_size = element_size * old_count;
    size_t new_size = element_size * new_count;

    void* new_ptr = allocate(new_size);
    if (old_ptr && old_count > 0) {
        std::memcpy(new_ptr, old_ptr, std::min(old_size, new_size));
    }
    // Old memory is not individually freed (pool manages it)
    return new_ptr;
}

void PoolAllocator::clear() {
    for (auto& chunk : chunks_) {
        std::free(chunk.data);
    }
    chunks_.clear();
    total_allocated_ = 0;
}

void PoolAllocator::add_chunk(size_t min_size) {
    size_t alloc_size = std::max(chunk_size_, min_size);
    // Cache-line align the allocation
    alloc_size = (alloc_size + CACHE_LINE - 1) & ~(CACHE_LINE - 1);

    Chunk chunk;
    chunk.data = static_cast<uint8_t*>(std::aligned_alloc(CACHE_LINE, alloc_size));
    if (!chunk.data) {
        throw std::bad_alloc();
    }
    chunk.capacity = alloc_size;
    chunk.used = 0;
    chunks_.push_back(chunk);
}

} // namespace pictor
