// SingleThreadDispatcher: the inline job dispatcher used where worker threads
// cannot start (Emscripten without pthreads; multi-rhi.md §6 S0).

#include "pictor/update/single_thread_dispatcher.h"
#include "test_common.h"

#include <utility>
#include <vector>

using namespace pictor_test;
using pictor::SingleThreadDispatcher;

int main() {
    SingleThreadDispatcher d;
    PT_ASSERT_OP(d.worker_count(), ==, 1u, "one worker");

    std::vector<std::pair<uint32_t, uint32_t>> ranges;
    d.dispatch(10, 4, [&](uint32_t s, uint32_t e) { ranges.emplace_back(s, e); });
    d.wait_all();
    PT_ASSERT_OP(ranges.size(), ==, 3u, "ceil(10/4) chunks");
    if (ranges.size() == 3) {
        PT_ASSERT(ranges[0] == std::make_pair(0u, 4u), "chunk 0");
        PT_ASSERT(ranges[1] == std::make_pair(4u, 8u), "chunk 1");
        PT_ASSERT(ranges[2] == std::make_pair(8u, 10u), "last chunk clipped");
    }

    ranges.clear();
    d.dispatch(0, 4, [&](uint32_t s, uint32_t e) { ranges.emplace_back(s, e); });
    PT_ASSERT(ranges.empty(), "empty range runs nothing");

    d.dispatch(5, 0, [&](uint32_t s, uint32_t e) { ranges.emplace_back(s, e); });
    PT_ASSERT_OP(ranges.size(), ==, 1u, "chunk 0 means one job");

    return report("unit_single_thread_dispatcher_test");
}
