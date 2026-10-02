#include "pictor/core/float_parse.h"

#include <cerrno>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <locale.h>
#include <string>

#if defined(__APPLE__)
#include <xlocale.h>
#endif

// Pick the C-locale conversion the platform offers:
//   _WIN32                : _strtod_l + _create_locale
//   Apple / glibc         : strtod_l + newlocale
//   other POSIX (bionic…) : uselocale + strtod (strtod_l is not universally
//                           available, e.g. bionic before API 26)
#if defined(_WIN32)
#define PICTOR_FLOAT_PARSE_WIN32 1
#elif defined(__APPLE__) || defined(__GLIBC__)
#define PICTOR_FLOAT_PARSE_STRTOD_L 1
#else
#define PICTOR_FLOAT_PARSE_USELOCALE 1
#endif

namespace pictor::float_parse {

namespace {

bool is_digit(char c) { return c >= '0' && c <= '9'; }

bool is_alnum_or_underscore(char c) {
    return is_digit(c) || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

// Case-insensitive match of the lowercase ASCII `word` at [p, last).
bool match_word(const char* p, const char* last, const char* word) {
    const size_t n = std::strlen(word);
    if (static_cast<size_t>(last - p) < n) return false;
    for (size_t i = 0; i < n; ++i) {
        char c = p[i];
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        if (c != word[i]) return false;
    }
    return true;
}

// Returns one past the longest prefix of [first, last) matching the
// std::from_chars `general` grammar, or nullptr when nothing matches.
// Validating here keeps strtod's extra leniency (leading whitespace, '+',
// hex floats) out of the accepted language.
const char* scan_general(const char* first, const char* last) {
    const char* p = first;
    if (p < last && *p == '-') ++p;

    if (match_word(p, last, "inf")) {
        p += 3;
        if (match_word(p, last, "inity")) p += 5;
        return p;
    }
    if (match_word(p, last, "nan")) {
        p += 3;
        if (p < last && *p == '(') {
            const char* q = p + 1;
            while (q < last && is_alnum_or_underscore(*q)) ++q;
            if (q < last && *q == ')') return q + 1;
        }
        return p;
    }

    size_t digits = 0;
    while (p < last && is_digit(*p)) { ++p; ++digits; }
    if (p < last && *p == '.') {
        ++p;
        while (p < last && is_digit(*p)) { ++p; ++digits; }
    }
    if (digits == 0) return nullptr;

    if (p < last && (*p == 'e' || *p == 'E')) {
        const char* q = p + 1;
        if (q < last && (*q == '+' || *q == '-')) ++q;
        if (q < last && is_digit(*q)) {
            while (q < last && is_digit(*q)) ++q;
            p = q;
        }
    }
    return p;
}

#if defined(PICTOR_FLOAT_PARSE_WIN32)
_locale_t c_numeric_locale() {
    static const _locale_t loc = _create_locale(LC_NUMERIC, "C");
    return loc;
}
#else
locale_t c_numeric_locale() {
    static const locale_t loc = newlocale(LC_NUMERIC_MASK, "C", static_cast<locale_t>(0));
    return loc;
}
#endif

// Converts a NUL-terminated buffer in the C locale.
double strtod_c_locale(const char* str, char** end) {
#if defined(PICTOR_FLOAT_PARSE_WIN32)
    if (const _locale_t loc = c_numeric_locale()) return _strtod_l(str, end, loc);
    return std::strtod(str, end);
#elif defined(PICTOR_FLOAT_PARSE_STRTOD_L)
    if (const locale_t loc = c_numeric_locale()) return strtod_l(str, end, loc);
    return std::strtod(str, end);
#else
    const locale_t loc = c_numeric_locale();
    if (!loc) return std::strtod(str, end);
    const locale_t previous = uselocale(loc);
    const double value = std::strtod(str, end);
    uselocale(previous);
    return value;
#endif
}

} // namespace

ParseResult parse_double(const char* first, const char* last, double& value) {
    const char* const match_end = (first && first < last) ? scan_general(first, last) : nullptr;
    if (!match_end) return {first, std::errc::invalid_argument};

    // strtod needs a NUL-terminated string; copy only the validated span.
    const size_t length = static_cast<size_t>(match_end - first);
    char        small[128];
    std::string large;
    char*       buffer = small;
    if (length >= sizeof(small)) {
        large.assign(first, length);
        buffer = large.data();
    } else {
        std::memcpy(small, first, length);
        small[length] = '\0';
    }

    const int saved_errno = errno;
    errno = 0;
    char* end = nullptr;
    const double parsed = strtod_c_locale(buffer, &end);
    const bool range_error = (errno == ERANGE);
    errno = saved_errno;

    const size_t consumed = static_cast<size_t>(end - buffer);
    if (consumed == 0) return {first, std::errc::invalid_argument};
    const char* const ptr = first + consumed;

    // ERANGE also flags subnormal results; only overflow to infinity and
    // underflow to zero are range errors under from_chars.
    if (range_error && (std::isinf(parsed) || parsed == 0.0))
        return {ptr, std::errc::result_out_of_range};

    value = parsed;
    return {ptr, std::errc{}};
}

} // namespace pictor::float_parse
