#include "memory/aligned_block.h"

#include <cstdlib>

#ifdef _MSC_VER
#include <malloc.h>
#endif

namespace pictor {

void* aligned_block_alloc(size_t alignment, size_t size) noexcept {
#ifdef _MSC_VER
    return _aligned_malloc(size, alignment);
#else
    void* block = nullptr;
    if (posix_memalign(&block, alignment, size) != 0) return nullptr;
    return block;
#endif
}

void aligned_block_free(void* block) noexcept {
#ifdef _MSC_VER
    _aligned_free(block);
#else
    std::free(block);
#endif
}

} // namespace pictor
