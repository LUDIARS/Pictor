#include "pictor/update/single_thread_dispatcher.h"

namespace pictor {

void SingleThreadDispatcher::dispatch(uint32_t count, uint32_t chunk_size, JobFunction fn) {
    if (count == 0 || !fn) return;
    if (chunk_size == 0) chunk_size = count;
    // Same chunk boundaries as ThreadPoolDispatcher, executed in order.
    for (uint32_t start = 0; start < count; ) {
        const uint32_t end = (count - start > chunk_size) ? start + chunk_size : count;
        fn(start, end);
        start = end;
    }
}

} // namespace pictor
