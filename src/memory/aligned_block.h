#pragma once

#include <cstddef>

namespace pictor {

/// Aligned heap block for the CPU allocators (FrameAllocator / PoolAllocator).
///
/// One place for the platform split instead of an #if per allocator:
///   MSVC          : _aligned_malloc / _aligned_free
///   everything else (Linux, Android, Apple, Emscripten):
///                   posix_memalign / free
/// std::aligned_alloc is not used: Android only has it from API 28, MSVC never,
/// and it requires `size` to be a multiple of `alignment`.
///
/// `alignment` must be a power of two and a multiple of sizeof(void*).
/// Returns nullptr on failure; free only with aligned_block_free.
void* aligned_block_alloc(size_t alignment, size_t size) noexcept;
void  aligned_block_free(void* block) noexcept;

} // namespace pictor
