// lorica_applied_tu_test — this translation unit is compiled the way every Pictor target is
// (cmake/PictorLorica.cmake, pictor_lorica_apply_all) and checks, from inside the unit, that
// Lorica's settings reached it. A build where the Lorica wiring silently fell off (the target
// no longer goes through pictor_lorica_target, a hardening flag is cancelled, the forced
// include is gone) still compiles, and this test then fails.
//
// The expectations come from tests/lorica_applied/CMakeLists.txt, which derives them from the
// toolchain the way Lorica's LoricaHardening.cmake (pinned commit) decides them:
//   PICTOR_LORICA_EXPECT_HARDENING  1 where Lorica hardens (Linux GCC/Clang, Windows MSVC)
//   PICTOR_LORICA_EXPECT_UNSAFE_API 1 where lorica_deprecate_unsafe_apis is applied
//   PICTOR_LORICA_EXPECT_FORTIFY    level, defined only in optimized configurations on Linux
// Pictor takes the baseline profile only (no EXTENDED target), so no EXTENDED marker is checked.

// ---------------------------------------------------------------------------------------------
// MUST stay above every #include. lorica/unsafe_api.hpp leaves no macro behind (#pragma once,
// its helper macros are #undef'd), so its mark is the declaration it adds: on non-Windows it
// declares ::strcpy ahead of the unit's first line. Before any header of our own, ::strcpy can
// only come from a forced include. Overload resolution picks the non-template ::strcpy when it
// is declared, else the fallback template below.
#if defined(__cplusplus) && !defined(_WIN32) && (defined(__GNUC__) || defined(__clang__))
namespace pictor_lorica_probe {
struct undeclared {};
template <class... Args> undeclared strcpy(Args...);
template <class T> T value_of();  // unevaluated stand-in, like std::declval
template <class T, class U> struct same { static constexpr bool value = false; };
template <class T> struct same<T, T> { static constexpr bool value = true; };
} // namespace pictor_lorica_probe
namespace pictor_lorica_lookup {
using namespace pictor_lorica_probe;
// Naming the deprecated function, even unevaluated, is what the forced header turns into an
// error; the probe only asks whether the declaration exists.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
constexpr bool kUnsafeApiHeaderForceIncluded =
    !pictor_lorica_probe::same<decltype(strcpy(pictor_lorica_probe::value_of<char*>(),
                                               pictor_lorica_probe::value_of<const char*>())),
                               pictor_lorica_probe::undeclared>::value;
#pragma GCC diagnostic pop
} // namespace pictor_lorica_lookup
#define PICTOR_LORICA_PROBE_FORCED_INCLUDE 1
#endif
// ---------------------------------------------------------------------------------------------

#include "test_common.h"

#include <lorica/checked_integer.hpp>
#include <lorica/input_budget.hpp>

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <limits>
#include <version>

#ifndef PICTOR_LORICA_EXPECT_HARDENING
#error "tests/lorica_applied/CMakeLists.txt must define PICTOR_LORICA_EXPECT_HARDENING"
#endif
#ifndef PICTOR_LORICA_EXPECT_UNSAFE_API
#error "tests/lorica_applied/CMakeLists.txt must define PICTOR_LORICA_EXPECT_UNSAFE_API"
#endif

namespace {

// Lorica::boundary is reachable only through the PRIVATE link pictor_lorica_target adds; the
// include above already fails to compile without it. Run the real code with values the
// compiler cannot fold away.
void check_boundary() {
    volatile std::size_t max_size = std::numeric_limits<std::size_t>::max();
    volatile std::size_t one = 1;
    const std::size_t left = max_size;
    const std::size_t right = one;
    PT_ASSERT(!lorica::checked_add(left, right).has_value(),
              "lorica::checked_add must reject size_t overflow");
    const auto sum = lorica::checked_add(left - right, right);
    PT_ASSERT(sum.has_value() && *sum == left, "lorica::checked_add must accept max - 1 + 1");

    volatile std::size_t limit_source = 4096;
    const std::size_t limit = limit_source;
    lorica::input_budget exact({limit, 8, 8, 4});
    PT_ASSERT(exact.consume(limit, 8, 8), "lorica::input_budget must accept exactly its limit");
    PT_ASSERT(!exact.consume(1, 0, 0), "an exhausted lorica::input_budget must refuse more");
    lorica::input_budget over({limit, 8, 8, 4});
    PT_ASSERT(!over.consume(limit + 1, 0, 0), "lorica::input_budget must refuse limit + 1 bytes");
    PT_ASSERT(!over.consume(0, 9, 0), "lorica::input_budget must refuse limit + 1 elements");
    PT_ASSERT(!over.consume(0, 0, 9), "lorica::input_budget must refuse limit + 1 allocations");
    PT_ASSERT(over.permits_depth(4) && !over.permits_depth(5),
              "lorica::input_budget depth limit is inclusive");
}

#if PICTOR_LORICA_EXPECT_HARDENING
void check_hardening() {
#if defined(_MSC_VER) && !defined(__clang__)
    // MSVC: Lorica's baseline is /GS /sdl /guard:cf (and /we4996 for the unsafe API
    // rejection). None of them defines a macro, so this unit cannot see them; the
    // compile command test (lorica_applied_compile_commands_test) reads them where the
    // generator writes compile_commands.json, and verify_binary.py checks the PE flags.
    std::printf("[INFO] MSVC: baseline flags are not visible from inside the unit\n");
#else
    // Stack protector: -fstack-protector-strong defines __SSP_STRONG__ (GCC and Clang).
#if defined(__SSP_STRONG__) || defined(__SSP_ALL__)
    constexpr bool ssp = true;
#else
    constexpr bool ssp = false;
#endif
    PT_ASSERT(ssp, "-fstack-protector-strong did not reach this unit (__SSP_STRONG__ undefined)");

    // Control-flow protection: x86 -fcf-protection=full -> __CET__ == 3; AArch64
    // -mbranch-protection=standard -> BTI and PAC defaults. -fstack-clash-protection
    // defines nothing; the compile command test covers it.
#if defined(__x86_64__) || defined(__i386__)
#if defined(__CET__) && __CET__ == 3
    constexpr bool cfp = true;
#else
    constexpr bool cfp = false;
#endif
    PT_ASSERT(cfp, "-fcf-protection=full did not reach this unit (__CET__ != 3)");
#elif defined(__aarch64__)
#if defined(__ARM_FEATURE_BTI_DEFAULT) && defined(__ARM_FEATURE_PAC_DEFAULT)
    constexpr bool cfp = true;
#else
    constexpr bool cfp = false;
#endif
    PT_ASSERT(cfp, "-mbranch-protection=standard did not reach this unit");
#endif

    // Position independent executable (POSITION_INDEPENDENT_CODE on every hardened target).
#if defined(__PIE__) || defined(__pie__)
    constexpr bool pie = true;
#else
    constexpr bool pie = false;
#endif
    PT_ASSERT(pie, "the executable is not compiled as PIE");

    // Standard library checks. Lorica defines both macros; the one the library reads decides.
#if !defined(_LIBCPP_HARDENING_MODE)
    PT_ASSERT(false, "_LIBCPP_HARDENING_MODE is not defined (Lorica defines it for every C++ unit)");
#endif
#if defined(_LIBCPP_VERSION)
#if _LIBCPP_HARDENING_MODE == _LIBCPP_HARDENING_MODE_FAST || \
    _LIBCPP_HARDENING_MODE == _LIBCPP_HARDENING_MODE_EXTENSIVE
    constexpr bool stdlib_checks = true;
#else
    constexpr bool stdlib_checks = false;
#endif
    PT_ASSERT(stdlib_checks, "libc++ hardening mode is not Lorica's (FAST / EXTENSIVE)");
#elif defined(__GLIBCXX__)
#if defined(_GLIBCXX_ASSERTIONS)
    constexpr bool stdlib_checks = true;
#else
    constexpr bool stdlib_checks = false;
#endif
    PT_ASSERT(stdlib_checks, "_GLIBCXX_ASSERTIONS is not defined");
#else
    PT_ASSERT(false, "unknown C++ standard library; Lorica supports libstdc++ and libc++");
#endif

    // FORTIFY: Lorica adds -U/-D _FORTIFY_SOURCE in Release / RelWithDebInfo / MinSizeRel
    // only, at 3 for compilers >= 12 (else 2). Other configurations (Debug, an empty build
    // type) get none, so nothing is expected there.
#if defined(PICTOR_LORICA_EXPECT_FORTIFY)
#if defined(_FORTIFY_SOURCE) && _FORTIFY_SOURCE == PICTOR_LORICA_EXPECT_FORTIFY
    constexpr bool fortify_requested = true;
#else
    constexpr bool fortify_requested = false;
#endif
    PT_ASSERT(fortify_requested, "_FORTIFY_SOURCE is not Lorica's level in this configuration");
#if defined(__GLIBC__)
    // glibc's effective level (it needs optimization); Lorica probes exactly this.
#if defined(__USE_FORTIFY_LEVEL) && __USE_FORTIFY_LEVEL == PICTOR_LORICA_EXPECT_FORTIFY
    constexpr bool fortify_effective = true;
#else
    constexpr bool fortify_effective = false;
#endif
    PT_ASSERT(fortify_effective, "glibc's effective FORTIFY level (__USE_FORTIFY_LEVEL) is not Lorica's");
#endif
#else
    std::printf("[INFO] no FORTIFY expected in this configuration (not optimized)\n");
#endif
#endif // MSVC

#if PICTOR_LORICA_EXPECT_UNSAFE_API
#if defined(PICTOR_LORICA_PROBE_FORCED_INCLUDE)
    PT_ASSERT(pictor_lorica_lookup::kUnsafeApiHeaderForceIncluded,
              "lorica/unsafe_api.hpp was not force-included ahead of the unit");
#elif defined(_WIN32)
    // Windows: lorica_deprecate_unsafe_apis adds /we4996 and no header; the UCRT's own
    // deprecations are what it makes errors. Nothing marks the unit; the compile command
    // test checks /we4996.
    std::printf("[INFO] Windows: unsafe API rejection (/we4996) is not visible from inside the unit\n");
#else
    PT_ASSERT(false, "no probe for the forced unsafe API header on this compiler");
#endif
#else
    std::printf("[INFO] Lorica's unsafe API rejection is not supported on this toolchain\n");
#endif
}
#endif

} // namespace

int main() {
    check_boundary();
#if PICTOR_LORICA_EXPECT_HARDENING
    check_hardening();
    return pictor_test::report("lorica_applied_tu_test");
#else
    // Lorica does not harden this toolchain (cmake/PictorLorica.cmake warns at configure).
    // The boundary checks above still ran; the hardening part is reported as not run.
    const int result = pictor_test::report("lorica_applied_tu_test (boundary only)");
    if (result == 0) {
        std::printf("PICTOR_LORICA_SKIP: Lorica hardening is not applied on this toolchain\n");
    }
    return result;
#endif
}
