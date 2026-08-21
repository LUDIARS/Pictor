#include "visus_json.h"

#include "pictor/core/parse_limits.h"

#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <system_error>

namespace pictor::visus_json {

namespace {

// Visus metadata is untrusted JSON and materializes every value. Bound both
// the source and the expanded value count to avoid small, flat inputs causing
// disproportionate heap growth.
constexpr size_t kMaxJsonValues     = 100'000;

// Per-object key count. `VisusMetadata` stores every key twice (ordered flat
// vector for round-trip output + lookup index), so a flat ~1 MB object stays
// under kMaxJsonValues yet expands to several times its source size. Convention
// keys number under 20 (§2.2), so 1024 leaves ample room for host-specific
// metadata.
constexpr size_t kMaxObjectMembers  = 1024;

// ---- encode ---------------------------------------------------------------

void emit_number(std::string& out, double v) {
    if (!std::isfinite(v)) { out += "0"; return; }
    char buf[64];
    const auto result = std::to_chars(buf, buf + sizeof(buf), v,
                                      std::chars_format::general,
                                      std::numeric_limits<double>::max_digits10);
    if (result.ec != std::errc{}) { out += "0"; return; }
    out.append(buf, static_cast<size_t>(result.ptr - buf));
}

void pad(std::string& out, int indent) {
    out.append(static_cast<size_t>(indent) * 2, ' ');
}

// ---- parser ---------------------------------------------------------------

struct Parser {
    const char* p;
    const char* end;
    std::string err;
    int         depth = 0;
    size_t      value_count = 0;

    bool fail(const char* what) {
        if (err.empty()) err = what;
        return false;
    }

    void skip_ws() {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) ++p;
    }

    bool consume(char c) {
        skip_ws();
        if (p < end && *p == c) { ++p; return true; }
        return false;
    }

    bool expect(char c) {
        if (!consume(c)) return fail("unexpected character");
        return true;
    }

    static void append_utf8(std::string& out, unsigned code) {
        if (code < 0x80) {
            out.push_back(static_cast<char>(code));
        } else if (code < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (code >> 6)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        } else if (code < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (code >> 12)));
            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (code >> 18)));
            out.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        }
    }

    bool parse_hex4(unsigned& code) {
        if (p + 4 > end) return fail("bad \\u escape");
        code = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = p[i];
            unsigned digit = 0;
            if (c >= '0' && c <= '9') digit = static_cast<unsigned>(c - '0');
            else if (c >= 'a' && c <= 'f') digit = static_cast<unsigned>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') digit = static_cast<unsigned>(c - 'A' + 10);
            else return fail("bad \\u escape");
            code = (code << 4) | digit;
        }
        p += 4;
        return true;
    }

    bool parse_string(std::string& out) {
        skip_ws();
        if (p >= end || *p != '"') return fail("expected string");
        ++p;
        out.clear();
        while (p < end && *p != '"') {
            if (*p == '\\') {
                ++p;
                if (p >= end) return fail("bad escape");
                switch (*p) {
                    case '"':  out.push_back('"');  ++p; break;
                    case '\\': out.push_back('\\'); ++p; break;
                    case '/':  out.push_back('/');  ++p; break;
                    case 'b':  out.push_back('\b'); ++p; break;
                    case 'f':  out.push_back('\f'); ++p; break;
                    case 'n':  out.push_back('\n'); ++p; break;
                    case 'r':  out.push_back('\r'); ++p; break;
                    case 't':  out.push_back('\t'); ++p; break;
                    case 'u': {
                        ++p;
                        unsigned code = 0;
                        if (!parse_hex4(code)) return false;
                        if (code >= 0xD800 && code <= 0xDBFF) {
                            if (p + 6 > end || p[0] != '\\' || p[1] != 'u')
                                return fail("unpaired high surrogate");
                            p += 2;
                            unsigned low = 0;
                            if (!parse_hex4(low)) return false;
                            if (low < 0xDC00 || low > 0xDFFF)
                                return fail("unpaired high surrogate");
                            code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
                        }
                        if (code >= 0xDC00 && code <= 0xDFFF)
                            return fail("unpaired low surrogate");
                        append_utf8(out, code);
                        break;
                    }
                    default: return fail("bad escape");
                }
            } else {
                if (static_cast<unsigned char>(*p) < 0x20)
                    return fail("unescaped control character");
                out.push_back(*p++);
            }
        }
        if (p >= end || *p != '"') return fail("unterminated string");
        ++p;
        return true;
    }

    bool parse_number(double& out) {
        skip_ws();
        if (p >= end) return fail("expected number");
        const char* start = p;
        if (*p == '-') ++p;
        if (p >= end) return fail("expected number");

        if (*p == '0') {
            ++p;
            if (p < end && *p >= '0' && *p <= '9')
                return fail("leading zero in number");
        } else if (*p >= '1' && *p <= '9') {
            while (p < end && *p >= '0' && *p <= '9') ++p;
        } else {
            return fail("expected number");
        }

        if (p < end && *p == '.') {
            ++p;
            if (p >= end || *p < '0' || *p > '9')
                return fail("expected fraction digits");
            while (p < end && *p >= '0' && *p <= '9') ++p;
        }
        if (p < end && (*p == 'e' || *p == 'E')) {
            ++p;
            if (p < end && (*p == '+' || *p == '-')) ++p;
            if (p >= end || *p < '0' || *p > '9')
                return fail("expected exponent digits");
            while (p < end && *p >= '0' && *p <= '9') ++p;
        }

        const auto result = std::from_chars(start, p, out, std::chars_format::general);
        if (result.ec != std::errc{} || result.ptr != p || !std::isfinite(out))
            return fail("number out of range");
        return true;
    }

    bool parse_value(VisusValue& out) {
        skip_ws();
        if (p >= end) return fail("unexpected end");
        if (++value_count > kMaxJsonValues) return fail("too many json values");
        switch (*p) {
            case '"': {
                std::string s;
                if (!parse_string(s)) return false;
                out = VisusValue(std::move(s));
                return true;
            }
            case '{':
            case '[': {
                if (++depth > parse_limits::kMaxNestingDepth)
                    return fail("json nesting too deep");
                const bool ok = (*p == '{') ? parse_object(out) : parse_array(out);
                --depth;
                return ok;
            }
            case 't':
                if (p + 4 <= end && std::memcmp(p, "true", 4) == 0) {
                    p += 4; out = VisusValue(true); return true;
                }
                return fail("bad literal");
            case 'f':
                if (p + 5 <= end && std::memcmp(p, "false", 5) == 0) {
                    p += 5; out = VisusValue(false); return true;
                }
                return fail("bad literal");
            case 'n':
                if (p + 4 <= end && std::memcmp(p, "null", 4) == 0) {
                    p += 4; out = VisusValue(); return true;
                }
                return fail("bad literal");
            default: {
                double d = 0.0;
                if (!parse_number(d)) return false;
                out = VisusValue(d);
                return true;
            }
        }
    }

    bool parse_object(VisusValue& out) {
        if (!expect('{')) return false;
        VisusMetadata& obj = out.mutable_object();
        skip_ws();
        if (consume('}')) return true;
        while (true) {
            std::string key;
            if (!parse_string(key)) return false;
            // Duplicate keys would make the document's meaning depend on which
            // one wins. `obj` already indexes the keys inserted so far, so this
            // needs no second key container.
            if (obj.has(key)) return fail("duplicate object key");
            if (!expect(':')) return false;
            VisusValue v;
            if (!parse_value(v)) return false;
            obj.set(std::move(key), std::move(v));
            if (obj.size() > kMaxObjectMembers) return fail("too many object members");
            if (consume(',')) continue;
            if (consume('}')) return true;
            return fail("expected , or }");
        }
    }

    bool parse_array(VisusValue& out) {
        if (!expect('[')) return false;
        VisusValue::Array& arr = out.mutable_array();
        skip_ws();
        if (consume(']')) return true;
        while (true) {
            VisusValue v;
            if (!parse_value(v)) return false;
            arr.push_back(std::move(v));
            if (consume(',')) continue;
            if (consume(']')) return true;
            return fail("expected , or ]");
        }
    }
};

} // namespace

// ---- public ---------------------------------------------------------------

bool parse(std::string_view text, VisusValue& out, std::string* error) {
    if (text.size() > kMaxInputBytes) {
        if (error) *error = "json input too large";
        return false;
    }
    Parser pr{text.data(), text.data() + text.size(), {}, 0, 0};
    VisusValue v;
    bool ok = pr.parse_value(v);
    if (ok) {
        pr.skip_ws();
        if (pr.p != pr.end) ok = pr.fail("trailing characters");
    }
    if (!ok) {
        if (error) *error = pr.err.empty() ? "parse error" : pr.err;
        return false;
    }
    out = std::move(v);
    return true;
}

namespace {

void emit_quoted(std::string& out, std::string_view s) {
    out.push_back('"');
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char esc[8];
                    std::snprintf(esc, sizeof(esc), "\\u%04x",
                                  static_cast<unsigned>(static_cast<unsigned char>(c)));
                    out += esc;
                } else {
                    out.push_back(c);
                }
        }
    }
    out.push_back('"');
}

void emit_value(std::string& out, const VisusValue& value, int indent) {
    switch (value.type()) {
        case VisusValue::Type::NUL:    out += "null"; return;
        case VisusValue::Type::BOOL:   out += value.as_bool() ? "true" : "false"; return;
        case VisusValue::Type::NUMBER: emit_number(out, value.as_number()); return;
        case VisusValue::Type::STRING: emit_quoted(out, value.as_string()); return;
        case VisusValue::Type::ARRAY: {
            const VisusValue::Array& arr = value.as_array();
            if (arr.empty()) { out += "[]"; return; }
            out += "[\n";
            for (size_t i = 0; i < arr.size(); ++i) {
                pad(out, indent + 1);
                emit_value(out, arr[i], indent + 1);
                if (i + 1 < arr.size()) out += ",";
                out += "\n";
            }
            pad(out, indent);
            out += "]";
            return;
        }
        case VisusValue::Type::OBJECT: {
            const VisusMetadata& obj = value.as_object();
            if (obj.empty()) { out += "{}"; return; }
            out += "{\n";
            size_t i = 0;
            for (const auto& [k, v] : obj) {
                pad(out, indent + 1);
                emit_quoted(out, k);
                out += ": ";
                emit_value(out, v, indent + 1);
                if (++i < obj.size()) out += ",";
                out += "\n";
            }
            pad(out, indent);
            out += "}";
            return;
        }
    }
}

} // namespace

std::string emit(const VisusValue& value) {
    std::string out;
    emit_value(out, value, 0);
    out += "\n";
    return out;
}

} // namespace pictor::visus_json
