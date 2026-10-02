// float_parse: std::from_chars 互換のロケール非依存な浮動小数点解析の契約。

#include "pictor/core/float_parse.h"
#include "pictor/visus/visus_serializer.h"
#include "test_common.h"

#include <clocale>
#include <cmath>
#include <cstring>
#include <limits>
#include <string>

using namespace pictor;
namespace fp = pictor::float_parse;

namespace {

struct Parsed {
    fp::ParseResult result;
    double          value;
    size_t          consumed;
};

// value は呼び出し前に番兵を入れ、 エラー時に書き換わらないことを見る。
constexpr double kSentinel = 12345.0;

Parsed parse(const char* text) {
    const size_t n = std::strlen(text);
    Parsed out{{nullptr, std::errc{}}, kSentinel, 0};
    out.result   = fp::parse_double(text, text + n, out.value);
    out.consumed = static_cast<size_t>(out.result.ptr - text);
    return out;
}

bool ok_full(const char* text, double expected) {
    const Parsed p = parse(text);
    return p.result.ec == std::errc{} && p.consumed == std::strlen(text) && p.value == expected;
}

void test_plain_values() {
    PT_ASSERT(ok_full("0", 0.0), "zero");
    PT_ASSERT(ok_full("1.5", 1.5), "decimal");
    PT_ASSERT(ok_full("42", 42.0), "integer");
    PT_ASSERT(ok_full("0.125", 0.125), "fraction");
    PT_ASSERT(ok_full("1.", 1.0), "trailing dot is part of the number");
    PT_ASSERT(ok_full(".5", 0.5), "leading dot");
    PT_ASSERT(ok_full("0.1", 0.1), "0.1 rounds like the compiler literal");
}

void test_exponent() {
    PT_ASSERT(ok_full("1e3", 1000.0), "lowercase exponent");
    PT_ASSERT(ok_full("2.5E-2", 0.025), "uppercase negative exponent");
    PT_ASSERT(ok_full("1e+2", 100.0), "explicit positive exponent");
    PT_ASSERT(ok_full("0e400", 0.0), "true zero with huge exponent is not underflow");
}

void test_negative() {
    PT_ASSERT(ok_full("-3.25", -3.25), "negative decimal");
    PT_ASSERT(ok_full("-1e-3", -0.001), "negative with exponent");
    const Parsed z = parse("-0");
    PT_ASSERT(z.result.ec == std::errc{} && z.value == 0.0 && std::signbit(z.value),
              "negative zero keeps its sign");
}

void test_trailing_characters_stop_parse() {
    Parsed p = parse("1.5abc");
    PT_ASSERT(p.result.ec == std::errc{} && p.consumed == 3 && p.value == 1.5,
              "stops before trailing letters");

    p = parse("2e");
    PT_ASSERT(p.result.ec == std::errc{} && p.consumed == 1 && p.value == 2.0,
              "incomplete exponent is not consumed");

    p = parse("3e+x");
    PT_ASSERT(p.result.ec == std::errc{} && p.consumed == 1 && p.value == 3.0,
              "exponent sign without digits is not consumed");

    p = parse("7,");
    PT_ASSERT(p.result.ec == std::errc{} && p.consumed == 1, "stops at a comma");

    // [first, last) 外は読まない: NUL 終端でない範囲の途中で止める。
    const char buf[] = {'1', '2', '3', '4'};
    double v = kSentinel;
    const auto r = fp::parse_double(buf, buf + 2, v);
    PT_ASSERT(r.ec == std::errc{} && r.ptr == buf + 2 && v == 12.0,
              "reads only the given range");
}

void test_invalid_input() {
    Parsed p = parse("");
    PT_ASSERT(p.result.ec == std::errc::invalid_argument && p.consumed == 0 && p.value == kSentinel,
              "empty string is invalid and leaves value untouched");

    const char* const invalid[] = {" 1", "+1", "-", ".", "e5", "abc", ",5", "-+1", "\t2"};
    for (const char* text : invalid) {
        p = parse(text);
        PT_ASSERT(p.result.ec == std::errc::invalid_argument && p.consumed == 0 &&
                      p.value == kSentinel,
                  text);
    }

    // hex float は general 書式では受けない ("0" だけ読む)。
    p = parse("0x1p3");
    PT_ASSERT(p.result.ec == std::errc{} && p.consumed == 1 && p.value == 0.0,
              "hex prefix is not accepted");

    double v = kSentinel;
    const auto r = fp::parse_double(nullptr, nullptr, v);
    PT_ASSERT(r.ec == std::errc::invalid_argument && v == kSentinel, "null range is invalid");
}

void test_other_locale_separator() {
    Parsed p = parse("1,5");
    PT_ASSERT(p.result.ec == std::errc{} && p.consumed == 1 && p.value == 1.0,
              "comma is not a decimal separator");

    // 小数点が ',' のロケールを有効にしても結果は変わらない (環境にあれば)。
    const char* const comma_locales[] = {"de_DE.UTF-8", "de_DE.utf8", "fr_FR.UTF-8",
                                         "German_Germany.1252", "de-DE"};
    const std::string previous = std::setlocale(LC_NUMERIC, nullptr);
    for (const char* name : comma_locales) {
        if (!std::setlocale(LC_NUMERIC, name)) continue;
        PT_ASSERT(ok_full("1.5", 1.5), "dot still parses under a comma locale");
        p = parse("1,5");
        PT_ASSERT(p.result.ec == std::errc{} && p.consumed == 1 && p.value == 1.0,
                  "comma still rejected under a comma locale");
        break;
    }
    std::setlocale(LC_NUMERIC, previous.c_str());
}

void test_range_errors() {
    Parsed p = parse("1e400");
    PT_ASSERT(p.result.ec == std::errc::result_out_of_range && p.consumed == 5 &&
                  p.value == kSentinel,
              "overflow is out of range and leaves value untouched");

    p = parse("-1e400");
    PT_ASSERT(p.result.ec == std::errc::result_out_of_range, "negative overflow");

    p = parse("1e-400");
    PT_ASSERT(p.result.ec == std::errc::result_out_of_range && p.consumed == 6 &&
                  p.value == kSentinel,
              "underflow to zero is out of range");

    p = parse("-1e-400");
    PT_ASSERT(p.result.ec == std::errc::result_out_of_range, "negative underflow");

    p = parse("4.9e-324");
    PT_ASSERT(p.result.ec == std::errc{} && p.value == std::numeric_limits<double>::denorm_min(),
              "smallest subnormal is accepted");

    PT_ASSERT(ok_full("1.7976931348623157e308", std::numeric_limits<double>::max()),
              "largest finite double");
}

void test_special_values() {
    Parsed p = parse("inf");
    PT_ASSERT(p.result.ec == std::errc{} && p.consumed == 3 && std::isinf(p.value), "inf");
    p = parse("-Infinity");
    PT_ASSERT(p.result.ec == std::errc{} && p.consumed == 9 && std::isinf(p.value) &&
                  p.value < 0.0,
              "-Infinity");
    p = parse("nan");
    PT_ASSERT(p.result.ec == std::errc{} && p.consumed == 3 && std::isnan(p.value), "nan");
}

void test_long_input_uses_heap_buffer() {
    std::string text = "1";
    text.append(300, '0');
    text += "e-300";
    double v = kSentinel;
    const auto r = fp::parse_double(text.data(), text.data() + text.size(), v);
    PT_ASSERT(r.ec == std::errc{} && r.ptr == text.data() + text.size() && v == 1.0,
              "long mantissa parses exactly");
}

// Visus JSON 経由の既存挙動 (受理 / 拒否) を保つ。
void test_visus_json_numbers() {
    VisusDesc   out;
    std::string err;
    PT_ASSERT(from_visus_json("{\"name\":\"x\",\"_n\":[1.5,-2e3,0,4.9e-324]}", out, &err),
              "valid json numbers parse");

    err.clear();
    PT_ASSERT(!from_visus_json("{\"name\":\"x\",\"_n\":1e400}", out, &err), "json overflow fails");
    PT_ASSERT(err.find("out of range") != std::string::npos, "overflow reports out of range");

    err.clear();
    PT_ASSERT(!from_visus_json("{\"name\":\"x\",\"_n\":1e-400}", out, &err), "json underflow fails");
    PT_ASSERT(err.find("out of range") != std::string::npos, "underflow reports out of range");

    PT_ASSERT(!from_visus_json("{\"name\":\"x\",\"_n\":+1}", out, &err), "json rejects '+'");
    PT_ASSERT(!from_visus_json("{\"name\":\"x\",\"_n\":1,5}", out, &err), "json rejects '1,5'");
}

} // namespace

int main() {
    test_plain_values();
    test_exponent();
    test_negative();
    test_trailing_characters_stop_parse();
    test_invalid_input();
    test_other_locale_separator();
    test_range_errors();
    test_special_values();
    test_long_input_uses_heap_buffer();
    test_visus_json_numbers();
    return pictor_test::report("unit_float_parse_test");
}
