#pragma once

// Locale-independent floating-point parsing with std::from_chars semantics.
//
// Floating-point std::from_chars is missing from some standard libraries
// (Apple libc++ in the macOS / iOS SDKs, the libc++ shipped with Android NDK
// r27 and older). This replacement keeps the from_chars contract so callers
// behave the same on every platform:
//
//   - reads only [first, last); the input need not be NUL terminated
//   - accepts the from_chars `general` grammar: optional '-', decimal digits
//     with an optional '.', optional exponent, or inf / infinity / nan[(...)]
//   - rejects leading whitespace, '+', hex floats and any decimal separator
//     other than '.' regardless of the current C / C++ locale
//   - returns the position one past the last character consumed
//   - errc::invalid_argument when no prefix matches, errc::result_out_of_range
//     when the value overflows to infinity or a non-zero value underflows to
//     zero; `value` is left untouched on either error. Subnormal results are
//     accepted.

#include <system_error>

namespace pictor::float_parse {

struct ParseResult {
    const char* ptr;
    std::errc   ec;
};

ParseResult parse_double(const char* first, const char* last, double& value);

} // namespace pictor::float_parse
