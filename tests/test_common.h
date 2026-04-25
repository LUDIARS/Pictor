#pragma once

#include <cstdio>
#include <cstdlib>

namespace pictor_test {

inline int g_failures = 0;

#define PT_ASSERT(cond, msg) do {                                    \
    if (!(cond)) {                                                   \
        std::fprintf(stderr, "[FAIL] %s:%d  %s  (%s)\n",             \
                     __FILE__, __LINE__, (msg), #cond);              \
        ::pictor_test::g_failures++;                                 \
    }                                                                \
} while (0)

#define PT_ASSERT_OP(a, op, b, msg) do {                             \
    auto _a = (a); auto _b = (b);                                    \
    if (!(_a op _b)) {                                               \
        std::fprintf(stderr, "[FAIL] %s:%d  %s  (%s %s %s)\n",       \
                     __FILE__, __LINE__, (msg), #a, #op, #b);        \
        std::fprintf(stderr, "       lhs = %lld, rhs = %lld\n",      \
                     static_cast<long long>(_a),                     \
                     static_cast<long long>(_b));                    \
        ::pictor_test::g_failures++;                                 \
    }                                                                \
} while (0)

inline int report(const char* test_name) {
    if (g_failures == 0) {
        std::printf("[PASS] %s\n", test_name);
        return 0;
    }
    std::fprintf(stderr, "[FAIL] %s — %d assertion(s) failed\n",
                 test_name, g_failures);
    return 1;
}

} // namespace pictor_test
