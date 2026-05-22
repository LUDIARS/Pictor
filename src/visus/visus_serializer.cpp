#include "pictor/visus/visus_serializer.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>

// 同 repo の material_serializer.cpp と同じ手書きパーサ方針。 外部依存なし。
// 未知 key は黙ってスキップ、 構文エラーのみ false 返却。

namespace pictor {

namespace {

// ---- enum ↔ string helpers ------------------------------------------------

const char* geometry_kind_to_str(VisusGeometryKind k) {
    switch (k) {
        case VisusGeometryKind::NONE:      return "none";
        case VisusGeometryKind::PRIMITIVE: return "primitive";
        case VisusGeometryKind::MODEL:     return "model";
        case VisusGeometryKind::RIVE:      return "rive";
        case VisusGeometryKind::UI:        return "ui";
        case VisusGeometryKind::PARTICLE:  return "particle";
        case VisusGeometryKind::TEXT:      return "text";
        case VisusGeometryKind::CUSTOM:    return "custom";
    }
    return "none";
}

VisusGeometryKind geometry_kind_from_str(std::string_view s) {
    if (s == "primitive") return VisusGeometryKind::PRIMITIVE;
    if (s == "model")     return VisusGeometryKind::MODEL;
    if (s == "rive")      return VisusGeometryKind::RIVE;
    if (s == "ui")        return VisusGeometryKind::UI;
    if (s == "particle")  return VisusGeometryKind::PARTICLE;
    if (s == "text")      return VisusGeometryKind::TEXT;
    if (s == "custom")    return VisusGeometryKind::CUSTOM;
    return VisusGeometryKind::NONE;
}

const char* fetch_policy_to_str(ResourceRef::FetchPolicy p) {
    switch (p) {
        case ResourceRef::FetchPolicy::LOCAL_ONLY:   return "local_only";
        case ResourceRef::FetchPolicy::CACHE_FIRST:  return "cache_first";
        case ResourceRef::FetchPolicy::REVALIDATE:   return "revalidate";
        case ResourceRef::FetchPolicy::ALWAYS_FETCH: return "always_fetch";
    }
    return "cache_first";
}

ResourceRef::FetchPolicy fetch_policy_from_str(std::string_view s) {
    if (s == "local_only")   return ResourceRef::FetchPolicy::LOCAL_ONLY;
    if (s == "revalidate")   return ResourceRef::FetchPolicy::REVALIDATE;
    if (s == "always_fetch") return ResourceRef::FetchPolicy::ALWAYS_FETCH;
    return ResourceRef::FetchPolicy::CACHE_FIRST;
}

const char* pool_to_str(PoolType p) {
    switch (p) {
        case PoolType::STATIC:     return "static";
        case PoolType::DYNAMIC:    return "dynamic";
        case PoolType::GPU_DRIVEN: return "gpu_driven";
    }
    return "dynamic";
}

PoolType pool_from_str(std::string_view s) {
    if (s == "static")     return PoolType::STATIC;
    if (s == "gpu_driven") return PoolType::GPU_DRIVEN;
    return PoolType::DYNAMIC;
}

const char* anim_kind_to_str(VisusAnimationDefault::Kind k) {
    switch (k) {
        case VisusAnimationDefault::Kind::NONE:               return "none";
        case VisusAnimationDefault::Kind::CLIP:               return "clip";
        case VisusAnimationDefault::Kind::STATE_MACHINE:      return "state_machine";
        case VisusAnimationDefault::Kind::RIVE_ANIMATION:     return "rive_animation";
        case VisusAnimationDefault::Kind::RIVE_STATE_MACHINE: return "rive_state_machine";
    }
    return "none";
}

VisusAnimationDefault::Kind anim_kind_from_str(std::string_view s) {
    if (s == "clip")               return VisusAnimationDefault::Kind::CLIP;
    if (s == "state_machine")      return VisusAnimationDefault::Kind::STATE_MACHINE;
    if (s == "rive_animation")     return VisusAnimationDefault::Kind::RIVE_ANIMATION;
    if (s == "rive_state_machine") return VisusAnimationDefault::Kind::RIVE_STATE_MACHINE;
    return VisusAnimationDefault::Kind::NONE;
}

// ---- vertex layout enums (§6.2) ------------------------------------------

const char* vertex_semantic_to_str(VertexSemantic s) {
    switch (s) {
        case VertexSemantic::POSITION:  return "position";
        case VertexSemantic::NORMAL:    return "normal";
        case VertexSemantic::TANGENT:   return "tangent";
        case VertexSemantic::TEXCOORD0: return "texcoord0";
        case VertexSemantic::TEXCOORD1: return "texcoord1";
        case VertexSemantic::COLOR0:    return "color0";
        case VertexSemantic::JOINTS:    return "joints";
        case VertexSemantic::WEIGHTS:   return "weights";
        case VertexSemantic::CUSTOM0:   return "custom0";
        case VertexSemantic::CUSTOM1:   return "custom1";
        case VertexSemantic::CUSTOM2:   return "custom2";
        case VertexSemantic::CUSTOM3:   return "custom3";
    }
    return "position";
}

VertexSemantic vertex_semantic_from_str(std::string_view s) {
    if (s == "normal")    return VertexSemantic::NORMAL;
    if (s == "tangent")   return VertexSemantic::TANGENT;
    if (s == "texcoord0") return VertexSemantic::TEXCOORD0;
    if (s == "texcoord1") return VertexSemantic::TEXCOORD1;
    if (s == "color0")    return VertexSemantic::COLOR0;
    if (s == "joints")    return VertexSemantic::JOINTS;
    if (s == "weights")   return VertexSemantic::WEIGHTS;
    if (s == "custom0")   return VertexSemantic::CUSTOM0;
    if (s == "custom1")   return VertexSemantic::CUSTOM1;
    if (s == "custom2")   return VertexSemantic::CUSTOM2;
    if (s == "custom3")   return VertexSemantic::CUSTOM3;
    return VertexSemantic::POSITION;
}

const char* vertex_attr_type_to_str(VertexAttributeType t) {
    switch (t) {
        case VertexAttributeType::FLOAT:    return "float";
        case VertexAttributeType::FLOAT2:   return "float2";
        case VertexAttributeType::FLOAT3:   return "float3";
        case VertexAttributeType::FLOAT4:   return "float4";
        case VertexAttributeType::UINT32:   return "uint32";
        case VertexAttributeType::INT32:    return "int32";
        case VertexAttributeType::UNORM8X4: return "unorm8x4";
        case VertexAttributeType::HALF2:    return "half2";
        case VertexAttributeType::HALF4:    return "half4";
        case VertexAttributeType::UINT32X4: return "uint32x4";
    }
    return "float";
}

VertexAttributeType vertex_attr_type_from_str(std::string_view s) {
    if (s == "float2")   return VertexAttributeType::FLOAT2;
    if (s == "float3")   return VertexAttributeType::FLOAT3;
    if (s == "float4")   return VertexAttributeType::FLOAT4;
    if (s == "uint32")   return VertexAttributeType::UINT32;
    if (s == "int32")    return VertexAttributeType::INT32;
    if (s == "unorm8x4") return VertexAttributeType::UNORM8X4;
    if (s == "half2")    return VertexAttributeType::HALF2;
    if (s == "half4")    return VertexAttributeType::HALF4;
    if (s == "uint32x4") return VertexAttributeType::UINT32X4;
    return VertexAttributeType::FLOAT;
}

// ---- handle ↔ string -----------------------------------------------------

constexpr uint32_t INVALID_HANDLE_U32 = std::numeric_limits<uint32_t>::max();

std::string handle_to_string(uint32_t h) {
    if (h == INVALID_HANDLE_U32) return "none";
    char buf[32];
    std::snprintf(buf, sizeof(buf), "handle:%u", static_cast<unsigned>(h));
    return buf;
}

uint32_t handle_from_string(std::string_view s) {
    if (s == "none" || s.empty()) return INVALID_HANDLE_U32;
    constexpr std::string_view prefix = "handle:";
    if (s.substr(0, prefix.size()) == prefix) s.remove_prefix(prefix.size());
    unsigned v = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return INVALID_HANDLE_U32;
        v = v * 10 + static_cast<unsigned>(c - '0');
    }
    return static_cast<uint32_t>(v);
}

// ---- encode helpers -------------------------------------------------------

void quote(std::string& out, std::string_view s) {
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

void emit_uint(std::string& out, uint64_t v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%llu",
                  static_cast<unsigned long long>(v));
    out += buf;
}

void emit_float(std::string& out, float v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%g", v);
    out += buf;
}

void emit_resource_ref(std::string& out, const ResourceRef& r, int indent) {
    auto pad = [&](int n) { out.append(static_cast<size_t>(n) * 2, ' '); };
    out += "{\n";
    pad(indent + 1); out += "\"local_path\":   "; quote(out, r.local_path);   out += ",\n";
    pad(indent + 1); out += "\"remote_url\":   "; quote(out, r.remote_url);   out += ",\n";
    pad(indent + 1); out += "\"sha256\":       "; quote(out, r.sha256);       out += ",\n";
    pad(indent + 1); out += "\"size_bytes\":   "; emit_uint(out, r.size_bytes); out += ",\n";
    pad(indent + 1); out += "\"fetch_policy\": "; quote(out, fetch_policy_to_str(r.fetch_policy)); out += ",\n";
    pad(indent + 1); out += "\"headers\":      [";
    if (!r.headers.empty()) {
        out += "\n";
        for (size_t i = 0; i < r.headers.size(); ++i) {
            pad(indent + 2);
            out += "{ \"name\": "; quote(out, r.headers[i].first);
            out += ", \"value\": "; quote(out, r.headers[i].second);
            out += " }";
            if (i + 1 < r.headers.size()) out += ",";
            out += "\n";
        }
        pad(indent + 1);
    }
    out += "]\n";
    pad(indent); out += "}";
}

// `VisusShaderStages::vertex_layout` を出力する (§6.2)。 空 layout でも
// `{ "stride": 0, "attributes": [] }` を書き、 round-trip を安定させる。
void emit_vertex_layout(std::string& out, const VertexLayout& vl, int indent) {
    auto pad = [&](int n) { out.append(static_cast<size_t>(n) * 2, ' '); };
    out += "{\n";
    pad(indent + 1); out += "\"stride\":     "; emit_uint(out, vl.stride); out += ",\n";
    pad(indent + 1); out += "\"attributes\": [";
    if (!vl.attributes.empty()) {
        out += "\n";
        for (size_t i = 0; i < vl.attributes.size(); ++i) {
            const VertexAttribute& a = vl.attributes[i];
            pad(indent + 2);
            out += "{ \"semantic\": "; quote(out, vertex_semantic_to_str(a.semantic));
            out += ", \"type\": ";     quote(out, vertex_attr_type_to_str(a.type));
            out += ", \"offset\": ";   emit_uint(out, a.offset);
            out += " }";
            if (i + 1 < vl.attributes.size()) out += ",";
            out += "\n";
        }
        pad(indent + 1);
    }
    out += "]\n";
    pad(indent); out += "}";
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
};

// ---- nested object parsers -----------------------------------------------

bool parse_resource_ref(Parser& pr, ResourceRef& out) {
    if (!pr.expect('{')) return false;
    pr.skip_ws();
    if (pr.consume('}')) return true;
    while (true) {
        std::string key;
        if (!pr.parse_string(key)) return false;
        if (!pr.expect(':')) return false;

        if (key == "local_path") {
            std::string s; if (pr.parse_string(s)) out.local_path = s;
        } else if (key == "remote_url") {
            std::string s; if (pr.parse_string(s)) out.remote_url = s;
        } else if (key == "sha256") {
            std::string s; if (pr.parse_string(s)) out.sha256 = s;
        } else if (key == "size_bytes") {
            double v; if (pr.parse_number(v)) out.size_bytes = static_cast<uint64_t>(v);
        } else if (key == "fetch_policy") {
            std::string s; if (pr.parse_string(s)) out.fetch_policy = fetch_policy_from_str(s);
        } else if (key == "headers") {
            if (!pr.expect('[')) return false;
            pr.skip_ws();
            if (!pr.consume(']')) {
                while (true) {
                    if (!pr.expect('{')) return false;
                    std::string h_name, h_value;
                    pr.skip_ws();
                    if (!pr.consume('}')) {
                        while (true) {
                            std::string hk;
                            if (!pr.parse_string(hk)) return false;
                            if (!pr.expect(':')) return false;
                            std::string hv;
                            if (pr.parse_string(hv)) {
                                if      (hk == "name")  h_name  = hv;
                                else if (hk == "value") h_value = hv;
                            } else {
                                pr.skip_value();
                            }
                            if (pr.consume(',')) continue;
                            if (pr.consume('}')) break;
                            return pr.fail("expected , or }");
                        }
                    }
                    if (!h_name.empty()) out.headers.emplace_back(h_name, h_value);
                    if (pr.consume(',')) continue;
                    if (pr.consume(']')) break;
                    return pr.fail("expected , or ]");
                }
            }
        } else {
            pr.skip_value();
        }

        if (pr.consume(',')) continue;
        if (pr.consume('}')) return true;
        return pr.fail("expected , or }");
    }
}

bool parse_vertex_attribute(Parser& pr, VertexAttribute& out) {
    if (!pr.expect('{')) return false;
    pr.skip_ws();
    if (pr.consume('}')) return true;
    while (true) {
        std::string key;
        if (!pr.parse_string(key)) return false;
        if (!pr.expect(':')) return false;

        if (key == "semantic") {
            std::string s; if (pr.parse_string(s)) out.semantic = vertex_semantic_from_str(s);
        } else if (key == "type") {
            std::string s; if (pr.parse_string(s)) out.type = vertex_attr_type_from_str(s);
        } else if (key == "offset") {
            double v; if (pr.parse_number(v)) out.offset = static_cast<uint16_t>(v);
        } else {
            pr.skip_value();
        }

        if (pr.consume(',')) continue;
        if (pr.consume('}')) return true;
        return pr.fail("expected , or }");
    }
}

bool parse_vertex_layout(Parser& pr, VertexLayout& out) {
    if (!pr.expect('{')) return false;
    pr.skip_ws();
    if (pr.consume('}')) return true;
    while (true) {
        std::string key;
        if (!pr.parse_string(key)) return false;
        if (!pr.expect(':')) return false;

        if (key == "stride") {
            double v; if (pr.parse_number(v)) out.stride = static_cast<uint32_t>(v);
        } else if (key == "attributes") {
            if (!pr.expect('[')) return false;
            pr.skip_ws();
            if (!pr.consume(']')) {
                while (true) {
                    VertexAttribute a{};
                    if (!parse_vertex_attribute(pr, a)) return false;
                    out.attributes.push_back(a);
                    if (pr.consume(',')) continue;
                    if (pr.consume(']')) break;
                    return pr.fail("expected , or ]");
                }
            }
        } else {
            pr.skip_value();
        }

        if (pr.consume(',')) continue;
        if (pr.consume('}')) return true;
        return pr.fail("expected , or }");
    }
}

bool parse_shader_stages(Parser& pr, VisusShaderStages& out) {
    if (!pr.expect('{')) return false;
    pr.skip_ws();
    if (pr.consume('}')) return true;
    while (true) {
        std::string key;
        if (!pr.parse_string(key)) return false;
        if (!pr.expect(':')) return false;

        if (key == "vert") {
            if (!parse_resource_ref(pr, out.vert)) return false;
        } else if (key == "frag") {
            if (!parse_resource_ref(pr, out.frag)) return false;
        } else if (key == "comp") {
            if (!parse_resource_ref(pr, out.comp)) return false;
        } else if (key == "vertex_layout") {
            if (!parse_vertex_layout(pr, out.vertex_layout)) return false;
        } else {
            pr.skip_value();
        }

        if (pr.consume(',')) continue;
        if (pr.consume('}')) return true;
        return pr.fail("expected , or }");
    }
}

bool parse_geometry(Parser& pr, VisusDesc& out) {
    if (!pr.expect('{')) return false;
    pr.skip_ws();
    if (pr.consume('}')) return true;
    while (true) {
        std::string key;
        if (!pr.parse_string(key)) return false;
        if (!pr.expect(':')) return false;

        if (key == "kind") {
            std::string s; if (pr.parse_string(s)) out.geometry_kind = geometry_kind_from_str(s);
        } else if (key == "asset") {
            if (!parse_resource_ref(pr, out.asset)) return false;
        } else if (key == "shader_stages") {
            if (!parse_shader_stages(pr, out.shader_stages)) return false;
        } else if (key == "rive_artboard") {
            std::string s; if (pr.parse_string(s)) out.rive_artboard = s;
        } else if (key == "text_default") {
            std::string s; if (pr.parse_string(s)) out.text_default = s;
        } else if (key == "mesh") {
            std::string s; if (pr.parse_string(s)) out.mesh = handle_from_string(s);
        } else if (key == "model") {
            std::string s; if (pr.parse_string(s)) out.model = handle_from_string(s);
        } else if (key == "shader") {
            std::string s; if (pr.parse_string(s)) out.shader = handle_from_string(s);
        } else if (key == "generic_handle") {
            double v; if (pr.parse_number(v)) out.generic_handle = static_cast<uint32_t>(v);
        } else {
            pr.skip_value();
        }

        if (pr.consume(',')) continue;
        if (pr.consume('}')) return true;
        return pr.fail("expected , or }");
    }
}

bool parse_material_slot(Parser& pr, VisusMaterialSlot& out) {
    if (!pr.expect('{')) return false;
    pr.skip_ws();
    if (pr.consume('}')) return true;
    while (true) {
        std::string key;
        if (!pr.parse_string(key)) return false;
        if (!pr.expect(':')) return false;

        if (key == "slot") {
            std::string s; if (pr.parse_string(s)) out.slot_name = s;
        } else if (key == "material") {
            std::string s; if (pr.parse_string(s)) out.material = handle_from_string(s);
        } else if (key == "resource") {
            if (!parse_resource_ref(pr, out.material_resource)) return false;
        } else {
            pr.skip_value();
        }

        if (pr.consume(',')) continue;
        if (pr.consume('}')) return true;
        return pr.fail("expected , or }");
    }
}

bool parse_texture_slot(Parser& pr, VisusTextureSlot& out) {
    if (!pr.expect('{')) return false;
    pr.skip_ws();
    if (pr.consume('}')) return true;
    while (true) {
        std::string key;
        if (!pr.parse_string(key)) return false;
        if (!pr.expect(':')) return false;

        if (key == "slot") {
            std::string s; if (pr.parse_string(s)) out.slot_name = s;
        } else if (key == "texture") {
            std::string s; if (pr.parse_string(s)) out.texture = handle_from_string(s);
        } else if (key == "resource") {
            if (!parse_resource_ref(pr, out.texture_resource)) return false;
        } else {
            pr.skip_value();
        }

        if (pr.consume(',')) continue;
        if (pr.consume('}')) return true;
        return pr.fail("expected , or }");
    }
}

bool parse_flags(Parser& pr, VisusDesc& out) {
    if (!pr.expect('{')) return false;
    pr.skip_ws();
    if (pr.consume('}')) return true;
    while (true) {
        std::string key;
        if (!pr.parse_string(key)) return false;
        if (!pr.expect(':')) return false;

        if (key == "default_flags") {
            double v; if (pr.parse_number(v)) out.default_flags = static_cast<uint16_t>(v);
        } else if (key == "layer") {
            double v; if (pr.parse_number(v)) out.layer = static_cast<uint16_t>(v);
        } else if (key == "pool_hint") {
            std::string s; if (pr.parse_string(s)) out.pool_hint = pool_from_str(s);
        } else if (key == "initial_lod") {
            double v; if (pr.parse_number(v)) out.initial_lod = static_cast<uint8_t>(v);
        } else {
            pr.skip_value();
        }

        if (pr.consume(',')) continue;
        if (pr.consume('}')) return true;
        return pr.fail("expected , or }");
    }
}

bool parse_animation_default(Parser& pr, VisusAnimationDefault& out) {
    if (!pr.expect('{')) return false;
    pr.skip_ws();
    if (pr.consume('}')) return true;
    while (true) {
        std::string key;
        if (!pr.parse_string(key)) return false;
        if (!pr.expect(':')) return false;

        if (key == "kind") {
            std::string s; if (pr.parse_string(s)) out.kind = anim_kind_from_str(s);
        } else if (key == "name") {
            std::string s; if (pr.parse_string(s)) out.name = s;
        } else if (key == "loop") {
            bool b; if (pr.parse_bool(b)) out.loop = b;
        } else if (key == "speed") {
            double v; if (pr.parse_number(v)) out.speed = static_cast<float>(v);
        } else {
            pr.skip_value();
        }

        if (pr.consume(',')) continue;
        if (pr.consume('}')) return true;
        return pr.fail("expected , or }");
    }
}

} // namespace

// ---- public API -----------------------------------------------------------

std::string to_visus_json(const VisusDesc& desc) {
    std::string out;
    out.reserve(2048);
    out += "{\n";
    out += "  \"version\": 1,\n";
    out += "  \"name\": ";   quote(out, desc.name); out += ",\n";

    // geometry
    out += "  \"geometry\": {\n";
    out += "    \"kind\":           "; quote(out, geometry_kind_to_str(desc.geometry_kind)); out += ",\n";
    out += "    \"asset\":          "; emit_resource_ref(out, desc.asset, 2); out += ",\n";
    out += "    \"shader_stages\": {\n";
    out += "      \"vert\": "; emit_resource_ref(out, desc.shader_stages.vert, 3); out += ",\n";
    out += "      \"frag\": "; emit_resource_ref(out, desc.shader_stages.frag, 3); out += ",\n";
    out += "      \"comp\": "; emit_resource_ref(out, desc.shader_stages.comp, 3); out += ",\n";
    out += "      \"vertex_layout\": ";
    emit_vertex_layout(out, desc.shader_stages.vertex_layout, 3); out += "\n";
    out += "    },\n";
    out += "    \"rive_artboard\":  "; quote(out, desc.rive_artboard); out += ",\n";
    out += "    \"text_default\":   "; quote(out, desc.text_default);  out += ",\n";
    out += "    \"mesh\":           "; quote(out, handle_to_string(desc.mesh));   out += ",\n";
    out += "    \"model\":          "; quote(out, handle_to_string(desc.model));  out += ",\n";
    out += "    \"shader\":         "; quote(out, handle_to_string(desc.shader)); out += ",\n";
    out += "    \"generic_handle\": "; emit_uint(out, desc.generic_handle); out += "\n";
    out += "  },\n";

    // materials
    out += "  \"materials\": [";
    if (!desc.materials.empty()) {
        out += "\n";
        for (size_t i = 0; i < desc.materials.size(); ++i) {
            const auto& m = desc.materials[i];
            out += "    {\n";
            out += "      \"slot\":     "; quote(out, m.slot_name); out += ",\n";
            out += "      \"material\": "; quote(out, handle_to_string(m.material)); out += ",\n";
            out += "      \"resource\": "; emit_resource_ref(out, m.material_resource, 3); out += "\n";
            out += "    }";
            if (i + 1 < desc.materials.size()) out += ",";
            out += "\n";
        }
        out += "  ";
    }
    out += "],\n";

    // textures
    out += "  \"textures\": [";
    if (!desc.textures.empty()) {
        out += "\n";
        for (size_t i = 0; i < desc.textures.size(); ++i) {
            const auto& t = desc.textures[i];
            out += "    {\n";
            out += "      \"slot\":     "; quote(out, t.slot_name); out += ",\n";
            out += "      \"texture\":  "; quote(out, handle_to_string(t.texture)); out += ",\n";
            out += "      \"resource\": "; emit_resource_ref(out, t.texture_resource, 3); out += "\n";
            out += "    }";
            if (i + 1 < desc.textures.size()) out += ",";
            out += "\n";
        }
        out += "  ";
    }
    out += "],\n";

    // flags
    out += "  \"flags\": {\n";
    out += "    \"default_flags\": "; emit_uint(out, desc.default_flags); out += ",\n";
    out += "    \"layer\":         "; emit_uint(out, desc.layer);         out += ",\n";
    out += "    \"pool_hint\":     "; quote(out, pool_to_str(desc.pool_hint)); out += ",\n";
    out += "    \"initial_lod\":   "; emit_uint(out, desc.initial_lod);   out += "\n";
    out += "  },\n";

    // animation_default
    out += "  \"animation_default\": {\n";
    out += "    \"kind\":  "; quote(out, anim_kind_to_str(desc.animation_default.kind)); out += ",\n";
    out += "    \"name\":  "; quote(out, desc.animation_default.name); out += ",\n";
    out += "    \"loop\":  "; out += desc.animation_default.loop ? "true" : "false"; out += ",\n";
    out += "    \"speed\": "; emit_float(out, desc.animation_default.speed); out += "\n";
    out += "  },\n";

    out += "  \"shader_key_override\": "; emit_uint(out, desc.shader_key_override); out += "\n";
    out += "}\n";
    return out;
}

bool from_visus_json(const std::string& json,
                     VisusDesc&         out,
                     std::string*       error)
{
    out = VisusDesc{};

    Parser pr{json.data(), json.data() + json.size(), {}};
    if (!pr.expect('{')) {
        if (error) *error = pr.err.empty() ? "expected object" : pr.err;
        return false;
    }
    pr.skip_ws();
    if (pr.consume('}')) return true;

    while (true) {
        std::string key;
        if (!pr.parse_string(key)) { if (error) *error = pr.err; return false; }
        if (!pr.expect(':'))       { if (error) *error = pr.err; return false; }

        if (key == "version") {
            double v; pr.parse_number(v);
        } else if (key == "name") {
            std::string s; if (pr.parse_string(s)) out.name = s;
        } else if (key == "geometry") {
            if (!parse_geometry(pr, out)) { if (error) *error = pr.err; return false; }
        } else if (key == "materials") {
            if (!pr.expect('[')) { if (error) *error = pr.err; return false; }
            pr.skip_ws();
            if (!pr.consume(']')) {
                while (true) {
                    VisusMaterialSlot slot;
                    if (!parse_material_slot(pr, slot)) { if (error) *error = pr.err; return false; }
                    out.materials.push_back(std::move(slot));
                    if (pr.consume(',')) continue;
                    if (pr.consume(']')) break;
                    if (error) *error = "expected , or ]";
                    return false;
                }
            }
        } else if (key == "textures") {
            if (!pr.expect('[')) { if (error) *error = pr.err; return false; }
            pr.skip_ws();
            if (!pr.consume(']')) {
                while (true) {
                    VisusTextureSlot slot;
                    if (!parse_texture_slot(pr, slot)) { if (error) *error = pr.err; return false; }
                    out.textures.push_back(std::move(slot));
                    if (pr.consume(',')) continue;
                    if (pr.consume(']')) break;
                    if (error) *error = "expected , or ]";
                    return false;
                }
            }
        } else if (key == "flags") {
            if (!parse_flags(pr, out)) { if (error) *error = pr.err; return false; }
        } else if (key == "animation_default") {
            if (!parse_animation_default(pr, out.animation_default)) {
                if (error) *error = pr.err;
                return false;
            }
        } else if (key == "shader_key_override") {
            double v; if (pr.parse_number(v)) out.shader_key_override = static_cast<uint64_t>(v);
        } else {
            pr.skip_value();
        }

        if (pr.consume(',')) continue;
        if (pr.consume('}')) return true;
        if (error) *error = "expected , or }";
        return false;
    }
}

} // namespace pictor
