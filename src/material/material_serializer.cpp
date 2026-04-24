#include "pictor/material/material_serializer.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>

// Minimal JSON encode / decode for MaterialDesc. No external dep.
// The parser is recursive-descent, permissive (unknown keys are
// silently skipped), and validates only the structural shapes
// MaterialDesc expects. MaterialDesc is a small, flat record so
// we don't need a general-purpose library.

namespace pictor {

namespace {

// ---- encode helpers -------------------------------------------------------

static std::string handle_to_string(TextureHandle h) {
    if (h == INVALID_TEXTURE) return "none";
    char buf[32];
    std::snprintf(buf, sizeof(buf), "handle:%u", static_cast<unsigned>(h));
    return buf;
}

static TextureHandle handle_from_string(std::string_view s) {
    if (s == "none" || s.empty()) return INVALID_TEXTURE;
    constexpr std::string_view prefix = "handle:";
    if (s.substr(0, prefix.size()) == prefix) s.remove_prefix(prefix.size());
    // Accept a bare integer too — tools may pass "42".
    unsigned v = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return INVALID_TEXTURE;
        v = v * 10 + static_cast<unsigned>(c - '0');
    }
    return static_cast<TextureHandle>(v);
}

static void quote(std::string& out, std::string_view s) {
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
                    std::snprintf(esc, sizeof(esc), "\\u%04x", c);
                    out += esc;
                } else {
                    out.push_back(c);
                }
        }
    }
    out.push_back('"');
}

static void emit_number(std::string& out, float v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%g", v);
    out += buf;
}

// ---- parser ---------------------------------------------------------------

struct Parser {
    const char* p;
    const char* end;
    std::string err;

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

    bool parse_string(std::string& out) {
        skip_ws();
        if (p >= end || *p != '"') return fail("expected string");
        ++p;
        out.clear();
        while (p < end && *p != '"') {
            if (*p == '\\' && p + 1 < end) {
                ++p;
                switch (*p) {
                    case '"':  out.push_back('"');  break;
                    case '\\': out.push_back('\\'); break;
                    case '/':  out.push_back('/');  break;
                    case 'b':  out.push_back('\b'); break;
                    case 'f':  out.push_back('\f'); break;
                    case 'n':  out.push_back('\n'); break;
                    case 'r':  out.push_back('\r'); break;
                    case 't':  out.push_back('\t'); break;
                    case 'u': {
                        if (p + 4 >= end) return fail("bad \\u escape");
                        // Minimal: emit the raw hex as a best-effort
                        // ASCII char; material JSON doesn't use \uXXXX.
                        char hex[5] = {p[1], p[2], p[3], p[4], 0};
                        p += 4;
                        unsigned code = std::strtoul(hex, nullptr, 16);
                        if (code < 0x80) out.push_back(static_cast<char>(code));
                        break;
                    }
                    default: return fail("bad escape");
                }
                ++p;
            } else {
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
        if (*p == '-' || *p == '+') ++p;
        while (p < end && ((*p >= '0' && *p <= '9') || *p == '.' ||
                           *p == 'e' || *p == 'E' || *p == '+' || *p == '-')) {
            ++p;
        }
        if (p == start) return fail("expected number");
        std::string s(start, static_cast<size_t>(p - start));
        out = std::atof(s.c_str());
        return true;
    }

    bool parse_bool(bool& out) {
        skip_ws();
        if (p + 4 <= end && std::memcmp(p, "true", 4) == 0) {
            p += 4; out = true; return true;
        }
        if (p + 5 <= end && std::memcmp(p, "false", 5) == 0) {
            p += 5; out = false; return true;
        }
        return fail("expected bool");
    }

    // Discards any JSON value — used to skip unknown keys.
    bool skip_value() {
        skip_ws();
        if (p >= end) return fail("unexpected end");
        switch (*p) {
            case '"': { std::string tmp; return parse_string(tmp); }
            case '{': return skip_object();
            case '[': return skip_array();
            case 't': case 'f': { bool tmp; return parse_bool(tmp); }
            case 'n':
                if (p + 4 <= end && std::memcmp(p, "null", 4) == 0) { p += 4; return true; }
                return fail("bad literal");
            default: { double tmp; return parse_number(tmp); }
        }
    }

    bool skip_object() {
        if (!expect('{')) return false;
        skip_ws();
        if (consume('}')) return true;
        while (true) {
            std::string key;
            if (!parse_string(key)) return false;
            if (!expect(':')) return false;
            if (!skip_value()) return false;
            if (consume(',')) continue;
            if (consume('}')) return true;
            return fail("expected , or }");
        }
    }

    bool skip_array() {
        if (!expect('[')) return false;
        skip_ws();
        if (consume(']')) return true;
        while (true) {
            if (!skip_value()) return false;
            if (consume(',')) continue;
            if (consume(']')) return true;
            return fail("expected , or ]");
        }
    }

    bool parse_number_array(float* out, size_t count) {
        if (!expect('[')) return false;
        for (size_t i = 0; i < count; ++i) {
            if (i > 0 && !expect(',')) return false;
            double v;
            if (!parse_number(v)) return false;
            out[i] = static_cast<float>(v);
        }
        return expect(']');
    }
};

// Apply one top-level key/value pair into `out`.
void apply_root_key(Parser& pr, const std::string& key,
                    MaterialDesc& out, std::string* name_out)
{
    if (key == "version") {
        double v; pr.parse_number(v); return;
    }
    if (key == "name") {
        std::string s;
        if (pr.parse_string(s) && name_out) *name_out = s;
        return;
    }
    if (key == "textures" || key == "params" || key == "flags") {
        // Parse nested object with the specific field set we expect.
        if (!pr.expect('{')) return;
        pr.skip_ws();
        if (pr.consume('}')) return;
        while (true) {
            std::string sub;
            if (!pr.parse_string(sub)) return;
            if (!pr.expect(':')) return;

            if (key == "textures") {
                std::string val;
                if (pr.parse_string(val)) {
                    TextureHandle h = handle_from_string(val);
                    if      (sub == "albedo")    out.albedo_texture    = h;
                    else if (sub == "normal")    out.normal_texture    = h;
                    else if (sub == "metallic")  out.metallic_texture  = h;
                    else if (sub == "roughness") out.roughness_texture = h;
                    else if (sub == "ao")        out.ao_texture        = h;
                    else if (sub == "emissive")  out.emissive_texture  = h;
                } else {
                    pr.skip_value();
                }
            } else if (key == "params") {
                if (sub == "base_color") {
                    pr.parse_number_array(out.base_color, 4);
                } else if (sub == "emissive") {
                    pr.parse_number_array(out.emissive, 3);
                } else {
                    double v;
                    if (pr.parse_number(v)) {
                        if      (sub == "metallic")     out.metallic     = static_cast<float>(v);
                        else if (sub == "roughness")    out.roughness    = static_cast<float>(v);
                        else if (sub == "alpha_cutoff") out.alpha_cutoff = static_cast<float>(v);
                        else if (sub == "normal_scale") out.normal_scale = static_cast<float>(v);
                        else if (sub == "ao_strength")  out.ao_strength  = static_cast<float>(v);
                    }
                }
            } else { // flags
                if (sub == "features") {
                    double v;
                    if (pr.parse_number(v)) out.features = static_cast<uint32_t>(v);
                } else {
                    bool b;
                    if (pr.parse_bool(b)) {
                        if      (sub == "cast_shadow")    out.cast_shadow    = b;
                        else if (sub == "receive_shadow") out.receive_shadow = b;
                    }
                }
            }

            if (pr.consume(',')) continue;
            if (pr.consume('}')) return;
            pr.fail("expected , or }");
            return;
        }
    }
    // Unknown root key: skip its value.
    pr.skip_value();
}

} // namespace

// ---- public API -----------------------------------------------------------

std::string to_material_json(const MaterialDesc& desc, const std::string& name) {
    std::string out;
    out.reserve(512);
    out += "{\n";
    out += "  \"version\": 1,\n";
    if (!name.empty()) {
        out += "  \"name\": ";
        quote(out, name);
        out += ",\n";
    }
    out += "  \"textures\": {\n";
    out += "    \"albedo\":    ";    quote(out, handle_to_string(desc.albedo_texture));    out += ",\n";
    out += "    \"normal\":    ";    quote(out, handle_to_string(desc.normal_texture));    out += ",\n";
    out += "    \"metallic\":  ";    quote(out, handle_to_string(desc.metallic_texture));  out += ",\n";
    out += "    \"roughness\": ";    quote(out, handle_to_string(desc.roughness_texture)); out += ",\n";
    out += "    \"ao\":        ";    quote(out, handle_to_string(desc.ao_texture));        out += ",\n";
    out += "    \"emissive\":  ";    quote(out, handle_to_string(desc.emissive_texture));  out += "\n";
    out += "  },\n";

    out += "  \"params\": {\n";
    out += "    \"base_color\": [";
    emit_number(out, desc.base_color[0]); out += ", ";
    emit_number(out, desc.base_color[1]); out += ", ";
    emit_number(out, desc.base_color[2]); out += ", ";
    emit_number(out, desc.base_color[3]); out += "],\n";
    out += "    \"emissive\": [";
    emit_number(out, desc.emissive[0]); out += ", ";
    emit_number(out, desc.emissive[1]); out += ", ";
    emit_number(out, desc.emissive[2]); out += "],\n";
    out += "    \"metallic\":     "; emit_number(out, desc.metallic);     out += ",\n";
    out += "    \"roughness\":    "; emit_number(out, desc.roughness);    out += ",\n";
    out += "    \"alpha_cutoff\": "; emit_number(out, desc.alpha_cutoff); out += ",\n";
    out += "    \"normal_scale\": "; emit_number(out, desc.normal_scale); out += ",\n";
    out += "    \"ao_strength\":  "; emit_number(out, desc.ao_strength);  out += "\n";
    out += "  },\n";

    out += "  \"flags\": {\n";
    out += "    \"cast_shadow\":    ";  out += desc.cast_shadow ? "true" : "false";    out += ",\n";
    out += "    \"receive_shadow\": ";  out += desc.receive_shadow ? "true" : "false"; out += ",\n";
    out += "    \"features\":       ";
    {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%u", static_cast<unsigned>(desc.features));
        out += buf;
    }
    out += "\n";
    out += "  }\n";
    out += "}\n";
    return out;
}

bool from_material_json(const std::string& json,
                        MaterialDesc&      out,
                        std::string*       name_out,
                        std::string*       error)
{
    out = MaterialDesc{};   // reset to defaults
    if (name_out) name_out->clear();

    Parser pr{json.data(), json.data() + json.size(), {}};
    if (!pr.expect('{')) {
        if (error) *error = pr.err.empty() ? "expected object" : pr.err;
        return false;
    }
    pr.skip_ws();
    if (pr.consume('}')) return true;

    while (true) {
        std::string key;
        if (!pr.parse_string(key)) {
            if (error) *error = pr.err;
            return false;
        }
        if (!pr.expect(':')) {
            if (error) *error = pr.err;
            return false;
        }
        apply_root_key(pr, key, out, name_out);
        if (pr.consume(',')) continue;
        if (pr.consume('}')) return true;
        if (error) *error = "expected , or }";
        return false;
    }
}

} // namespace pictor
