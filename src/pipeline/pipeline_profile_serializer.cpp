#include "pictor/pipeline/pipeline_profile_serializer.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>

// visus_serializer.cpp と同じ手書きパーサ方針。 外部依存なし。
// 未知 key は黙ってスキップ、 構文エラーのみ false 返却。

namespace pictor {

namespace {

constexpr uint32_t INVALID_HANDLE_U32 = std::numeric_limits<uint32_t>::max();

// ---- enum <-> string helpers ----------------------------------------------

const char* rendering_path_to_str(RenderingPath p) {
    switch (p) {
        case RenderingPath::FORWARD:      return "FORWARD";
        case RenderingPath::FORWARD_PLUS: return "FORWARD_PLUS";
        case RenderingPath::DEFERRED:     return "DEFERRED";
        case RenderingPath::HYBRID:       return "HYBRID";
    }
    return "FORWARD_PLUS";
}

RenderingPath rendering_path_from_str(std::string_view s, RenderingPath fallback) {
    if (s == "FORWARD")      return RenderingPath::FORWARD;
    if (s == "FORWARD_PLUS") return RenderingPath::FORWARD_PLUS;
    if (s == "DEFERRED")     return RenderingPath::DEFERRED;
    if (s == "HYBRID")       return RenderingPath::HYBRID;
    return fallback;
}

const char* pass_type_to_str(PassType t) {
    switch (t) {
        case PassType::DEPTH_ONLY:   return "DEPTH_ONLY";
        case PassType::OPAQUE:       return "OPAQUE";
        case PassType::TRANSPARENT:  return "TRANSPARENT";
        case PassType::SHADOW:       return "SHADOW";
        case PassType::POST_PROCESS: return "POST_PROCESS";
        case PassType::COMPUTE:      return "COMPUTE";
        case PassType::CUSTOM:       return "CUSTOM";
    }
    return "OPAQUE";
}

PassType pass_type_from_str(std::string_view s, PassType fallback) {
    if (s == "DEPTH_ONLY")   return PassType::DEPTH_ONLY;
    if (s == "OPAQUE")       return PassType::OPAQUE;
    if (s == "TRANSPARENT")  return PassType::TRANSPARENT;
    if (s == "SHADOW")       return PassType::SHADOW;
    if (s == "POST_PROCESS") return PassType::POST_PROCESS;
    if (s == "COMPUTE")      return PassType::COMPUTE;
    if (s == "CUSTOM")       return PassType::CUSTOM;
    return fallback;
}

const char* sort_mode_to_str(SortMode m) {
    switch (m) {
        case SortMode::FRONT_TO_BACK: return "FRONT_TO_BACK";
        case SortMode::BACK_TO_FRONT: return "BACK_TO_FRONT";
        case SortMode::NONE:          return "NONE";
    }
    return "FRONT_TO_BACK";
}

SortMode sort_mode_from_str(std::string_view s, SortMode fallback) {
    if (s == "FRONT_TO_BACK") return SortMode::FRONT_TO_BACK;
    if (s == "BACK_TO_FRONT") return SortMode::BACK_TO_FRONT;
    if (s == "NONE")          return SortMode::NONE;
    return fallback;
}

const char* shadow_filter_to_str(ShadowFilterMode m) {
    switch (m) {
        case ShadowFilterMode::NONE: return "NONE";
        case ShadowFilterMode::PCF:  return "PCF";
        case ShadowFilterMode::PCSS: return "PCSS";
    }
    return "PCF";
}

ShadowFilterMode shadow_filter_from_str(std::string_view s, ShadowFilterMode fallback) {
    if (s == "NONE") return ShadowFilterMode::NONE;
    if (s == "PCF")  return ShadowFilterMode::PCF;
    if (s == "PCSS") return ShadowFilterMode::PCSS;
    return fallback;
}

const char* post_process_kind_to_str(PostProcessKind k) {
    switch (k) {
        case PostProcessKind::UNKNOWN:        return "Unknown";
        case PostProcessKind::BLOOM:          return "Bloom";
        case PostProcessKind::TONE_MAPPING:   return "ToneMapping";
        case PostProcessKind::VIGNETTE:       return "Vignette";
        case PostProcessKind::COLOR_GRADING:  return "ColorGrading";
        case PostProcessKind::DEPTH_OF_FIELD: return "DepthOfField";
    }
    return "Unknown";
}

// Accepts the canonical token plus the same aliases as
// post_process_kind_from_name() so an explicit `kind` and a bare `name`
// resolve identically.
PostProcessKind post_process_kind_from_str(std::string_view s, PostProcessKind fallback) {
    if (s == "Bloom")          return PostProcessKind::BLOOM;
    if (s == "ToneMapping" || s == "Tonemapping" || s == "Tonemap" ||
        s == "tone_mapping")   return PostProcessKind::TONE_MAPPING;
    if (s == "Vignette")       return PostProcessKind::VIGNETTE;
    if (s == "ColorGrading" || s == "color_grading" || s == "LUT" ||
        s == "Grade")          return PostProcessKind::COLOR_GRADING;
    if (s == "DepthOfField" || s == "depth_of_field" || s == "DoF")
                               return PostProcessKind::DEPTH_OF_FIELD;
    if (s == "Unknown")        return PostProcessKind::UNKNOWN;
    return fallback;
}

const char* tone_map_operator_to_str(ToneMapOperator op) {
    switch (op) {
        case ToneMapOperator::ACES_FILMIC:  return "ACES_FILMIC";
        case ToneMapOperator::REINHARD:     return "REINHARD";
        case ToneMapOperator::REINHARD_EXT: return "REINHARD_EXT";
        case ToneMapOperator::UNCHARTED2:   return "UNCHARTED2";
        case ToneMapOperator::LINEAR_CLAMP: return "LINEAR_CLAMP";
    }
    return "ACES_FILMIC";
}

ToneMapOperator tone_map_operator_from_str(std::string_view s, ToneMapOperator fallback) {
    if (s == "ACES_FILMIC")  return ToneMapOperator::ACES_FILMIC;
    if (s == "REINHARD")     return ToneMapOperator::REINHARD;
    if (s == "REINHARD_EXT") return ToneMapOperator::REINHARD_EXT;
    if (s == "UNCHARTED2")   return ToneMapOperator::UNCHARTED2;
    if (s == "LINEAR_CLAMP") return ToneMapOperator::LINEAR_CLAMP;
    return fallback;
}

const char* overlay_mode_to_str(OverlayMode m) {
    switch (m) {
        case OverlayMode::OFF:      return "OFF";
        case OverlayMode::MINIMAL:  return "MINIMAL";
        case OverlayMode::STANDARD: return "STANDARD";
        case OverlayMode::DETAILED: return "DETAILED";
        case OverlayMode::TIMELINE: return "TIMELINE";
    }
    return "STANDARD";
}

OverlayMode overlay_mode_from_str(std::string_view s, OverlayMode fallback) {
    if (s == "OFF")      return OverlayMode::OFF;
    if (s == "MINIMAL")  return OverlayMode::MINIMAL;
    if (s == "STANDARD") return OverlayMode::STANDARD;
    if (s == "DETAILED") return OverlayMode::DETAILED;
    if (s == "TIMELINE") return OverlayMode::TIMELINE;
    return fallback;
}

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
    if (s.empty()) return INVALID_HANDLE_U32;
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
    std::snprintf(buf, sizeof(buf), "%llu", static_cast<unsigned long long>(v));
    out += buf;
}

void emit_bool(std::string& out, bool v) { out += v ? "true" : "false"; }

void emit_float(std::string& out, float v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%g", static_cast<double>(v));
    out += buf;
}

void emit_string_array(std::string& out, const std::vector<std::string>& arr) {
    out.push_back('[');
    for (size_t i = 0; i < arr.size(); ++i) {
        if (i) out += ", ";
        quote(out, arr[i]);
    }
    out.push_back(']');
}

void pad(std::string& out, int n) { out.append(static_cast<size_t>(n) * 2, ' '); }

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

    // -- typed helpers ------------------------------------------------------

    bool parse_uint_field(uint64_t& out) {
        double v;
        if (!parse_number(v)) return false;
        if (v < 0.0) v = 0.0;
        out = static_cast<uint64_t>(v);
        return true;
    }

    bool parse_float_field(float& out) {
        double v;
        if (!parse_number(v)) return false;
        out = static_cast<float>(v);
        return true;
    }

    // Parse a JSON array of strings into `out`. `out` is cleared first.
    bool parse_string_array(std::vector<std::string>& out) {
        if (!expect('[')) return false;
        out.clear();
        skip_ws();
        if (consume(']')) return true;
        while (true) {
            std::string s;
            if (!parse_string(s)) return false;
            out.push_back(std::move(s));
            if (consume(',')) continue;
            if (consume(']')) return true;
            return fail("expected , or ]");
        }
    }

    // Parse a JSON array of 3 numbers into a float3-like {x,y,z}.
    bool parse_float3(float& x, float& y, float& z) {
        if (!expect('[')) return false;
        double v;
        skip_ws();
        if (consume(']')) return true;
        if (!parse_number(v)) return false; x = static_cast<float>(v);
        if (consume(']')) return true;
        if (!expect(',')) return false;
        if (!parse_number(v)) return false; y = static_cast<float>(v);
        if (consume(']')) return true;
        if (!expect(',')) return false;
        if (!parse_number(v)) return false; z = static_cast<float>(v);
        if (consume(']')) return true;
        // tolerate trailing elements
        while (true) {
            if (!skip_value()) return false;
            if (consume(',')) continue;
            if (consume(']')) return true;
            return fail("expected , or ]");
        }
    }
};

// Generic object-member iteration: calls `on_key(parser, key)` for each
// member. `on_key` must consume exactly one value (or skip_value()).
template <typename Fn>
bool parse_object(Parser& pr, Fn&& on_key) {
    if (!pr.expect('{')) return false;
    pr.skip_ws();
    if (pr.consume('}')) return true;
    while (true) {
        std::string key;
        if (!pr.parse_string(key)) return false;
        if (!pr.expect(':')) return false;
        if (!on_key(pr, key)) return false;
        if (pr.consume(',')) continue;
        if (pr.consume('}')) return true;
        return pr.fail("expected , or }");
    }
}

// ---- nested object parsers ------------------------------------------------

bool parse_render_pass(Parser& pr, RenderPassDef& out) {
    return parse_object(pr, [&](Parser& p, const std::string& key) -> bool {
        if (key == "pass_name") {
            return p.parse_string(out.pass_name);
        } else if (key == "pass_type") {
            std::string s;
            if (!p.parse_string(s)) return false;
            out.pass_type = pass_type_from_str(s, out.pass_type);
            return true;
        } else if (key == "shader_override") {
            std::string s;
            if (!p.parse_string(s)) return false;
            out.shader_override = handle_from_string(s);
            return true;
        } else if (key == "render_targets") {
            return p.parse_string_array(out.render_targets);
        } else if (key == "input_textures") {
            return p.parse_string_array(out.input_textures);
        } else if (key == "sort_mode") {
            std::string s;
            if (!p.parse_string(s)) return false;
            out.sort_mode = sort_mode_from_str(s, out.sort_mode);
            return true;
        } else if (key == "filter_mask") {
            uint64_t v;
            if (!p.parse_uint_field(v)) return false;
            out.filter_mask = static_cast<uint16_t>(v);
            return true;
        } else if (key == "gpu_driven_pass") {
            return p.parse_bool(out.gpu_driven_pass);
        } else if (key == "required_streams") {
            return p.parse_string_array(out.required_streams);
        }
        return p.skip_value();
    });
}

// ---- post-process effect parameter parsers --------------------------------

bool parse_bloom_config(Parser& pr, BloomConfig& out) {
    return parse_object(pr, [&](Parser& p, const std::string& key) -> bool {
        if (key == "enabled")             return p.parse_bool(out.enabled);
        else if (key == "threshold")      return p.parse_float_field(out.threshold);
        else if (key == "soft_threshold") return p.parse_float_field(out.soft_threshold);
        else if (key == "intensity")      return p.parse_float_field(out.intensity);
        else if (key == "radius")         return p.parse_float_field(out.radius);
        else if (key == "mip_levels") {
            uint64_t v; if (!p.parse_uint_field(v)) return false;
            out.mip_levels = static_cast<uint32_t>(v); return true;
        } else if (key == "scatter")      return p.parse_float_field(out.scatter);
        return p.skip_value();
    });
}

bool parse_tone_mapping_config(Parser& pr, ToneMappingConfig& out) {
    return parse_object(pr, [&](Parser& p, const std::string& key) -> bool {
        if (key == "enabled")          return p.parse_bool(out.enabled);
        else if (key == "op") {
            std::string s; if (!p.parse_string(s)) return false;
            out.op = tone_map_operator_from_str(s, out.op); return true;
        } else if (key == "exposure")    return p.parse_float_field(out.exposure);
        else if (key == "gamma")         return p.parse_float_field(out.gamma);
        else if (key == "white_point")   return p.parse_float_field(out.white_point);
        else if (key == "saturation")    return p.parse_float_field(out.saturation);
        return p.skip_value();
    });
}

bool parse_vignette_config(Parser& pr, VignetteConfig& out) {
    return parse_object(pr, [&](Parser& p, const std::string& key) -> bool {
        if (key == "enabled")        return p.parse_bool(out.enabled);
        else if (key == "intensity") return p.parse_float_field(out.intensity);
        else if (key == "radius")    return p.parse_float_field(out.radius);
        else if (key == "softness")  return p.parse_float_field(out.softness);
        else if (key == "color")
            return p.parse_float3(out.color[0], out.color[1], out.color[2]);
        return p.skip_value();
    });
}

bool parse_color_grading_config(Parser& pr, ColorGradingConfig& out) {
    return parse_object(pr, [&](Parser& p, const std::string& key) -> bool {
        if (key == "enabled")            return p.parse_bool(out.enabled);
        else if (key == "lut_path")      return p.parse_string(out.lut_path);
        else if (key == "lut_intensity") return p.parse_float_field(out.lut_intensity);
        else if (key == "lut_size") {
            uint64_t v; if (!p.parse_uint_field(v)) return false;
            out.lut_size = static_cast<uint32_t>(v); return true;
        }
        return p.skip_value();
    });
}

bool parse_depth_of_field_config(Parser& pr, DepthOfFieldConfig& out) {
    return parse_object(pr, [&](Parser& p, const std::string& key) -> bool {
        if (key == "enabled")             return p.parse_bool(out.enabled);
        else if (key == "focus_distance") return p.parse_float_field(out.focus_distance);
        else if (key == "focus_range")    return p.parse_float_field(out.focus_range);
        else if (key == "bokeh_radius")   return p.parse_float_field(out.bokeh_radius);
        else if (key == "near_start")     return p.parse_float_field(out.near_start);
        else if (key == "near_end")       return p.parse_float_field(out.near_end);
        else if (key == "far_start")      return p.parse_float_field(out.far_start);
        else if (key == "far_end")        return p.parse_float_field(out.far_end);
        else if (key == "sample_count") {
            uint64_t v; if (!p.parse_uint_field(v)) return false;
            out.sample_count = static_cast<uint32_t>(v); return true;
        }
        return p.skip_value();
    });
}

bool parse_post_process(Parser& pr, PostProcessDef& out) {
    bool kind_explicit = false;
    bool ok = parse_object(pr, [&](Parser& p, const std::string& key) -> bool {
        if (key == "name") {
            return p.parse_string(out.name);
        } else if (key == "enabled") {
            return p.parse_bool(out.enabled);
        } else if (key == "kind") {
            std::string s;
            if (!p.parse_string(s)) return false;
            out.kind = post_process_kind_from_str(s, out.kind);
            kind_explicit = true;
            return true;
        } else if (key == "bloom") {
            return parse_bloom_config(p, out.bloom);
        } else if (key == "tone_mapping") {
            return parse_tone_mapping_config(p, out.tone_mapping);
        } else if (key == "vignette") {
            return parse_vignette_config(p, out.vignette);
        } else if (key == "color_grading") {
            return parse_color_grading_config(p, out.color_grading);
        } else if (key == "depth_of_field") {
            return parse_depth_of_field_config(p, out.depth_of_field);
        }
        // Unknown keys are skipped (forward-compat, spec §2).
        return p.skip_value();
    });
    if (!ok) return false;
    // No explicit `kind` -> infer from the effect name (preset spelling).
    if (!kind_explicit && out.kind == PostProcessKind::UNKNOWN) {
        out.kind = post_process_kind_from_name(out.name);
    }
    return true;
}

bool parse_shadow_config(Parser& pr, ShadowConfig& out) {
    return parse_object(pr, [&](Parser& p, const std::string& key) -> bool {
        if (key == "cascade_count") {
            uint64_t v; if (!p.parse_uint_field(v)) return false;
            out.cascade_count = static_cast<uint32_t>(v); return true;
        } else if (key == "resolution") {
            uint64_t v; if (!p.parse_uint_field(v)) return false;
            out.resolution = static_cast<uint32_t>(v); return true;
        } else if (key == "filter_mode") {
            std::string s; if (!p.parse_string(s)) return false;
            out.filter_mode = shadow_filter_from_str(s, out.filter_mode); return true;
        }
        return p.skip_value();
    });
}

bool parse_shadow_map_config(Parser& pr, ShadowMapConfig& out) {
    return parse_object(pr, [&](Parser& p, const std::string& key) -> bool {
        if (key == "cascade_count") {
            uint64_t v; if (!p.parse_uint_field(v)) return false;
            out.cascade_count = static_cast<uint32_t>(v); return true;
        } else if (key == "resolution") {
            uint64_t v; if (!p.parse_uint_field(v)) return false;
            out.resolution = static_cast<uint32_t>(v); return true;
        } else if (key == "depth_bias")          return p.parse_float_field(out.depth_bias);
        else if (key == "normal_bias")           return p.parse_float_field(out.normal_bias);
        else if (key == "slope_scale_bias")      return p.parse_float_field(out.slope_scale_bias);
        else if (key == "cascade_lambda")        return p.parse_float_field(out.cascade_lambda);
        else if (key == "max_shadow_dist")       return p.parse_float_field(out.max_shadow_dist);
        else if (key == "cascade_blend_width")   return p.parse_float_field(out.cascade_blend_width);
        else if (key == "shadow_strength")       return p.parse_float_field(out.shadow_strength);
        else if (key == "pcss_light_size")       return p.parse_float_field(out.pcss_light_size);
        else if (key == "pcss_min_penumbra")     return p.parse_float_field(out.pcss_min_penumbra);
        else if (key == "pcss_max_penumbra")     return p.parse_float_field(out.pcss_max_penumbra);
        else if (key == "pcss_blocker_search_radius")
            return p.parse_float_field(out.pcss_blocker_search_radius);
        else if (key == "filter_mode") {
            std::string s; if (!p.parse_string(s)) return false;
            out.filter_mode = shadow_filter_from_str(s, out.filter_mode); return true;
        }
        return p.skip_value();
    });
}

bool parse_ssao_config(Parser& pr, SSAOConfig& out) {
    return parse_object(pr, [&](Parser& p, const std::string& key) -> bool {
        if (key == "sample_count") {
            uint64_t v; if (!p.parse_uint_field(v)) return false;
            out.sample_count = static_cast<uint32_t>(v); return true;
        } else if (key == "radius")        return p.parse_float_field(out.radius);
        else if (key == "bias")            return p.parse_float_field(out.bias);
        else if (key == "intensity")       return p.parse_float_field(out.intensity);
        else if (key == "falloff_start")   return p.parse_float_field(out.falloff_start);
        else if (key == "falloff_end")     return p.parse_float_field(out.falloff_end);
        else if (key == "blur_enabled")    return p.parse_bool(out.blur_enabled);
        return p.skip_value();
    });
}

bool parse_gi_probe_config(Parser& pr, GIProbeConfig& out) {
    return parse_object(pr, [&](Parser& p, const std::string& key) -> bool {
        if (key == "grid_origin")
            return p.parse_float3(out.grid_origin.x, out.grid_origin.y, out.grid_origin.z);
        else if (key == "grid_spacing")
            return p.parse_float3(out.grid_spacing.x, out.grid_spacing.y, out.grid_spacing.z);
        else if (key == "grid_x") {
            uint64_t v; if (!p.parse_uint_field(v)) return false;
            out.grid_x = static_cast<uint32_t>(v); return true;
        } else if (key == "grid_y") {
            uint64_t v; if (!p.parse_uint_field(v)) return false;
            out.grid_y = static_cast<uint32_t>(v); return true;
        } else if (key == "grid_z") {
            uint64_t v; if (!p.parse_uint_field(v)) return false;
            out.grid_z = static_cast<uint32_t>(v); return true;
        } else if (key == "gi_intensity")        return p.parse_float_field(out.gi_intensity);
        else if (key == "max_probe_distance")    return p.parse_float_field(out.max_probe_distance);
        return p.skip_value();
    });
}

bool parse_gi_config(Parser& pr, GIConfig& out) {
    return parse_object(pr, [&](Parser& p, const std::string& key) -> bool {
        if (key == "shadow_enabled")        return p.parse_bool(out.shadow_enabled);
        else if (key == "ssao_enabled")     return p.parse_bool(out.ssao_enabled);
        else if (key == "gi_probes_enabled")return p.parse_bool(out.gi_probes_enabled);
        else if (key == "shadow")           return parse_shadow_map_config(p, out.shadow);
        else if (key == "ssao")             return parse_ssao_config(p, out.ssao);
        else if (key == "probes")           return parse_gi_probe_config(p, out.probes);
        return p.skip_value();
    });
}

bool parse_gpu_mem_config(Parser& pr, GpuMemoryAllocator::Config& out) {
    return parse_object(pr, [&](Parser& p, const std::string& key) -> bool {
        uint64_t v;
        if (key == "mesh_pool_size") {
            if (!p.parse_uint_field(v)) return false;
            out.mesh_pool_size = static_cast<size_t>(v); return true;
        } else if (key == "ssbo_pool_size") {
            if (!p.parse_uint_field(v)) return false;
            out.ssbo_pool_size = static_cast<size_t>(v); return true;
        } else if (key == "instance_buffer_size") {
            if (!p.parse_uint_field(v)) return false;
            out.instance_buffer_size = static_cast<size_t>(v); return true;
        } else if (key == "indirect_buffer_size") {
            if (!p.parse_uint_field(v)) return false;
            out.indirect_buffer_size = static_cast<size_t>(v); return true;
        } else if (key == "staging_buffer_size") {
            if (!p.parse_uint_field(v)) return false;
            out.staging_buffer_size = static_cast<size_t>(v); return true;
        }
        return p.skip_value();
    });
}

bool parse_memory_config(Parser& pr, MemoryConfig& out) {
    return parse_object(pr, [&](Parser& p, const std::string& key) -> bool {
        uint64_t v;
        if (key == "frame_allocator_size") {
            if (!p.parse_uint_field(v)) return false;
            out.frame_allocator_size = static_cast<size_t>(v); return true;
        } else if (key == "flight_count") {
            if (!p.parse_uint_field(v)) return false;
            out.flight_count = static_cast<uint32_t>(v); return true;
        } else if (key == "pool_chunk_size") {
            if (!p.parse_uint_field(v)) return false;
            out.pool_chunk_size = static_cast<size_t>(v); return true;
        } else if (key == "use_large_pages") {
            return p.parse_bool(out.use_large_pages);
        } else if (key == "gpu") {
            return parse_gpu_mem_config(p, out.gpu_config);
        }
        return p.skip_value();
    });
}

bool parse_gpu_driven_config(Parser& pr, GPUDrivenConfig& out) {
    return parse_object(pr, [&](Parser& p, const std::string& key) -> bool {
        uint64_t v;
        if (key == "max_triangle_count") {
            if (!p.parse_uint_field(v)) return false;
            out.max_triangle_count = static_cast<uint32_t>(v); return true;
        } else if (key == "min_instance_count") {
            if (!p.parse_uint_field(v)) return false;
            out.min_instance_count = static_cast<uint32_t>(v); return true;
        } else if (key == "workgroup_size") {
            if (!p.parse_uint_field(v)) return false;
            out.workgroup_size = static_cast<uint32_t>(v); return true;
        } else if (key == "two_phase_culling") {
            return p.parse_bool(out.two_phase_culling);
        } else if (key == "compute_update") {
            return p.parse_bool(out.compute_update);
        }
        return p.skip_value();
    });
}

bool parse_update_config(Parser& pr, UpdateConfig& out) {
    return parse_object(pr, [&](Parser& p, const std::string& key) -> bool {
        uint64_t v;
        if (key == "chunk_size") {
            if (!p.parse_uint_field(v)) return false;
            out.chunk_size = static_cast<uint32_t>(v); return true;
        } else if (key == "worker_threads") {
            if (!p.parse_uint_field(v)) return false;
            out.worker_threads = static_cast<uint32_t>(v); return true;
        } else if (key == "nt_store_enabled") {
            return p.parse_bool(out.nt_store_enabled);
        } else if (key == "nt_store_threshold") {
            if (!p.parse_uint_field(v)) return false;
            out.nt_store_threshold = static_cast<uint32_t>(v); return true;
        }
        return p.skip_value();
    });
}

bool parse_profiler_config(Parser& pr, ProfilerConfig& out) {
    return parse_object(pr, [&](Parser& p, const std::string& key) -> bool {
        if (key == "enabled") {
            return p.parse_bool(out.enabled);
        } else if (key == "overlay_mode") {
            std::string s; if (!p.parse_string(s)) return false;
            out.overlay_mode = overlay_mode_from_str(s, out.overlay_mode); return true;
        } else if (key == "max_queries") {
            uint64_t v; if (!p.parse_uint_field(v)) return false;
            out.max_queries = static_cast<uint32_t>(v); return true;
        }
        return p.skip_value();
    });
}

// ---- top-level decode -----------------------------------------------------

bool decode(const std::string& json, PipelineProfileDef& out, std::string* error) {
    Parser pr{json.data(), json.data() + json.size(), {}};

    auto fail = [&](const char* msg) -> bool {
        if (error) *error = pr.err.empty() ? msg : pr.err;
        return false;
    };

    if (!pr.expect('{')) return fail("expected object");
    pr.skip_ws();
    if (pr.consume('}')) return true; // empty object — keep preset/defaults

    while (true) {
        std::string key;
        if (!pr.parse_string(key)) return fail("expected key");
        if (!pr.expect(':'))       return fail("expected :");

        bool ok = true;
        if (key == "version") {
            double v; ok = pr.parse_number(v);
        } else if (key == "profile_name") {
            ok = pr.parse_string(out.profile_name);
        } else if (key == "rendering_path") {
            std::string s; ok = pr.parse_string(s);
            if (ok) out.rendering_path = rendering_path_from_str(s, out.rendering_path);
        } else if (key == "max_lights") {
            uint64_t v; ok = pr.parse_uint_field(v);
            if (ok) out.max_lights = static_cast<uint32_t>(v);
        } else if (key == "msaa_samples") {
            uint64_t v; ok = pr.parse_uint_field(v);
            if (ok) out.msaa_samples = static_cast<uint8_t>(v);
        } else if (key == "gpu_driven_enabled") {
            ok = pr.parse_bool(out.gpu_driven_enabled);
        } else if (key == "compute_update_enabled") {
            ok = pr.parse_bool(out.compute_update_enabled);
        } else if (key == "render_passes") {
            if (!pr.expect('[')) return fail("expected array");
            out.render_passes.clear();
            pr.skip_ws();
            if (!pr.consume(']')) {
                while (true) {
                    RenderPassDef rp;
                    if (!parse_render_pass(pr, rp)) return fail("bad render pass");
                    out.render_passes.push_back(std::move(rp));
                    if (pr.consume(',')) continue;
                    if (pr.consume(']')) break;
                    return fail("expected , or ]");
                }
            }
        } else if (key == "post_process") {
            if (!pr.expect('[')) return fail("expected array");
            out.post_process_stack.clear();
            pr.skip_ws();
            if (!pr.consume(']')) {
                while (true) {
                    PostProcessDef pp;
                    if (!parse_post_process(pr, pp)) return fail("bad post_process");
                    out.post_process_stack.push_back(std::move(pp));
                    if (pr.consume(',')) continue;
                    if (pr.consume(']')) break;
                    return fail("expected , or ]");
                }
            }
        } else if (key == "shadow") {
            ok = parse_shadow_config(pr, out.shadow_config);
        } else if (key == "gi") {
            ok = parse_gi_config(pr, out.gi_config);
        } else if (key == "memory") {
            ok = parse_memory_config(pr, out.memory_config);
        } else if (key == "gpu_driven") {
            ok = parse_gpu_driven_config(pr, out.gpu_driven_config);
        } else if (key == "update") {
            ok = parse_update_config(pr, out.update_config);
        } else if (key == "profiler") {
            ok = parse_profiler_config(pr, out.profiler_config);
        } else {
            ok = pr.skip_value();
        }

        if (!ok) return fail("parse error");

        if (pr.consume(',')) continue;
        if (pr.consume('}')) return true;
        return fail("expected , or }");
    }
}

} // namespace

// ============================================================
// Public API — encode
// ============================================================

std::string to_pipeline_profile_json(const PipelineProfileDef& def) {
    std::string out;
    out.reserve(4096);

    out += "{\n";
    out += "  \"version\": 1,\n";
    out += "  \"profile_name\": "; quote(out, def.profile_name); out += ",\n";
    out += "  \"rendering_path\": "; quote(out, rendering_path_to_str(def.rendering_path)); out += ",\n";
    out += "  \"max_lights\": "; emit_uint(out, def.max_lights); out += ",\n";
    out += "  \"msaa_samples\": "; emit_uint(out, def.msaa_samples); out += ",\n";
    out += "  \"gpu_driven_enabled\": "; emit_bool(out, def.gpu_driven_enabled); out += ",\n";
    out += "  \"compute_update_enabled\": "; emit_bool(out, def.compute_update_enabled); out += ",\n";

    // render_passes
    out += "  \"render_passes\": [";
    if (!def.render_passes.empty()) {
        out += "\n";
        for (size_t i = 0; i < def.render_passes.size(); ++i) {
            const auto& rp = def.render_passes[i];
            pad(out, 2); out += "{\n";
            pad(out, 3); out += "\"pass_name\":        "; quote(out, rp.pass_name); out += ",\n";
            pad(out, 3); out += "\"pass_type\":        "; quote(out, pass_type_to_str(rp.pass_type)); out += ",\n";
            pad(out, 3); out += "\"shader_override\":  "; quote(out, handle_to_string(rp.shader_override)); out += ",\n";
            pad(out, 3); out += "\"render_targets\":   "; emit_string_array(out, rp.render_targets); out += ",\n";
            pad(out, 3); out += "\"input_textures\":   "; emit_string_array(out, rp.input_textures); out += ",\n";
            pad(out, 3); out += "\"sort_mode\":        "; quote(out, sort_mode_to_str(rp.sort_mode)); out += ",\n";
            pad(out, 3); out += "\"filter_mask\":      "; emit_uint(out, rp.filter_mask); out += ",\n";
            pad(out, 3); out += "\"gpu_driven_pass\":  "; emit_bool(out, rp.gpu_driven_pass); out += ",\n";
            pad(out, 3); out += "\"required_streams\": "; emit_string_array(out, rp.required_streams); out += "\n";
            pad(out, 2); out += "}";
            if (i + 1 < def.render_passes.size()) out += ",";
            out += "\n";
        }
        pad(out, 1);
    }
    out += "],\n";

    // post_process
    out += "  \"post_process\": [";
    if (!def.post_process_stack.empty()) {
        out += "\n";
        for (size_t i = 0; i < def.post_process_stack.size(); ++i) {
            const auto& pp = def.post_process_stack[i];
            pad(out, 2); out += "{\n";
            pad(out, 3); out += "\"name\":    "; quote(out, pp.name); out += ",\n";
            pad(out, 3); out += "\"enabled\": "; emit_bool(out, pp.enabled); out += ",\n";
            pad(out, 3); out += "\"kind\":    ";
            quote(out, post_process_kind_to_str(pp.kind));

            // Emit only the parameter block matching `kind`. Effects with no
            // host-driven implementation (UNKNOWN) carry name/enabled/kind
            // only — their parameters live in the editor JSON, not here.
            switch (pp.kind) {
                case PostProcessKind::BLOOM: {
                    const auto& b = pp.bloom;
                    out += ",\n";
                    pad(out, 3); out += "\"bloom\": {\n";
                    pad(out, 4); out += "\"threshold\":      "; emit_float(out, b.threshold); out += ",\n";
                    pad(out, 4); out += "\"soft_threshold\": "; emit_float(out, b.soft_threshold); out += ",\n";
                    pad(out, 4); out += "\"intensity\":      "; emit_float(out, b.intensity); out += ",\n";
                    pad(out, 4); out += "\"radius\":         "; emit_float(out, b.radius); out += ",\n";
                    pad(out, 4); out += "\"mip_levels\":     "; emit_uint(out, b.mip_levels); out += ",\n";
                    pad(out, 4); out += "\"scatter\":        "; emit_float(out, b.scatter); out += "\n";
                    pad(out, 3); out += "}\n";
                    break;
                }
                case PostProcessKind::TONE_MAPPING: {
                    const auto& t = pp.tone_mapping;
                    out += ",\n";
                    pad(out, 3); out += "\"tone_mapping\": {\n";
                    pad(out, 4); out += "\"op\":          "; quote(out, tone_map_operator_to_str(t.op)); out += ",\n";
                    pad(out, 4); out += "\"exposure\":    "; emit_float(out, t.exposure); out += ",\n";
                    pad(out, 4); out += "\"gamma\":       "; emit_float(out, t.gamma); out += ",\n";
                    pad(out, 4); out += "\"white_point\": "; emit_float(out, t.white_point); out += ",\n";
                    pad(out, 4); out += "\"saturation\":  "; emit_float(out, t.saturation); out += "\n";
                    pad(out, 3); out += "}\n";
                    break;
                }
                case PostProcessKind::VIGNETTE: {
                    const auto& v = pp.vignette;
                    out += ",\n";
                    pad(out, 3); out += "\"vignette\": {\n";
                    pad(out, 4); out += "\"intensity\": "; emit_float(out, v.intensity); out += ",\n";
                    pad(out, 4); out += "\"radius\":    "; emit_float(out, v.radius); out += ",\n";
                    pad(out, 4); out += "\"softness\":  "; emit_float(out, v.softness); out += ",\n";
                    pad(out, 4); out += "\"color\":     [";
                    emit_float(out, v.color[0]); out += ", ";
                    emit_float(out, v.color[1]); out += ", ";
                    emit_float(out, v.color[2]); out += "]\n";
                    pad(out, 3); out += "}\n";
                    break;
                }
                case PostProcessKind::COLOR_GRADING: {
                    const auto& c = pp.color_grading;
                    out += ",\n";
                    pad(out, 3); out += "\"color_grading\": {\n";
                    pad(out, 4); out += "\"lut_path\":      "; quote(out, c.lut_path); out += ",\n";
                    pad(out, 4); out += "\"lut_intensity\": "; emit_float(out, c.lut_intensity); out += ",\n";
                    pad(out, 4); out += "\"lut_size\":      "; emit_uint(out, c.lut_size); out += "\n";
                    pad(out, 3); out += "}\n";
                    break;
                }
                case PostProcessKind::DEPTH_OF_FIELD: {
                    const auto& d = pp.depth_of_field;
                    out += ",\n";
                    pad(out, 3); out += "\"depth_of_field\": {\n";
                    pad(out, 4); out += "\"focus_distance\": "; emit_float(out, d.focus_distance); out += ",\n";
                    pad(out, 4); out += "\"focus_range\":    "; emit_float(out, d.focus_range); out += ",\n";
                    pad(out, 4); out += "\"bokeh_radius\":   "; emit_float(out, d.bokeh_radius); out += ",\n";
                    pad(out, 4); out += "\"near_start\":     "; emit_float(out, d.near_start); out += ",\n";
                    pad(out, 4); out += "\"near_end\":       "; emit_float(out, d.near_end); out += ",\n";
                    pad(out, 4); out += "\"far_start\":      "; emit_float(out, d.far_start); out += ",\n";
                    pad(out, 4); out += "\"far_end\":        "; emit_float(out, d.far_end); out += ",\n";
                    pad(out, 4); out += "\"sample_count\":   "; emit_uint(out, d.sample_count); out += "\n";
                    pad(out, 3); out += "}\n";
                    break;
                }
                case PostProcessKind::UNKNOWN:
                    out += "\n";
                    break;
            }
            pad(out, 2); out += "}";
            if (i + 1 < def.post_process_stack.size()) out += ",";
            out += "\n";
        }
        pad(out, 1);
    }
    out += "],\n";

    // shadow
    {
        const auto& s = def.shadow_config;
        out += "  \"shadow\": {\n";
        pad(out, 2); out += "\"cascade_count\": "; emit_uint(out, s.cascade_count); out += ",\n";
        pad(out, 2); out += "\"resolution\":    "; emit_uint(out, s.resolution); out += ",\n";
        pad(out, 2); out += "\"filter_mode\":   "; quote(out, shadow_filter_to_str(s.filter_mode)); out += "\n";
        out += "  },\n";
    }

    // gi
    {
        const auto& g = def.gi_config;
        out += "  \"gi\": {\n";
        pad(out, 2); out += "\"shadow_enabled\":    "; emit_bool(out, g.shadow_enabled); out += ",\n";
        pad(out, 2); out += "\"ssao_enabled\":      "; emit_bool(out, g.ssao_enabled); out += ",\n";
        pad(out, 2); out += "\"gi_probes_enabled\": "; emit_bool(out, g.gi_probes_enabled); out += ",\n";

        const auto& sh = g.shadow;
        pad(out, 2); out += "\"shadow\": {\n";
        pad(out, 3); out += "\"cascade_count\":              "; emit_uint(out, sh.cascade_count); out += ",\n";
        pad(out, 3); out += "\"resolution\":                 "; emit_uint(out, sh.resolution); out += ",\n";
        pad(out, 3); out += "\"depth_bias\":                 "; emit_float(out, sh.depth_bias); out += ",\n";
        pad(out, 3); out += "\"normal_bias\":                "; emit_float(out, sh.normal_bias); out += ",\n";
        pad(out, 3); out += "\"slope_scale_bias\":           "; emit_float(out, sh.slope_scale_bias); out += ",\n";
        pad(out, 3); out += "\"cascade_lambda\":             "; emit_float(out, sh.cascade_lambda); out += ",\n";
        pad(out, 3); out += "\"max_shadow_dist\":            "; emit_float(out, sh.max_shadow_dist); out += ",\n";
        pad(out, 3); out += "\"cascade_blend_width\":        "; emit_float(out, sh.cascade_blend_width); out += ",\n";
        pad(out, 3); out += "\"filter_mode\":                "; quote(out, shadow_filter_to_str(sh.filter_mode)); out += ",\n";
        pad(out, 3); out += "\"shadow_strength\":            "; emit_float(out, sh.shadow_strength); out += ",\n";
        pad(out, 3); out += "\"pcss_light_size\":            "; emit_float(out, sh.pcss_light_size); out += ",\n";
        pad(out, 3); out += "\"pcss_min_penumbra\":          "; emit_float(out, sh.pcss_min_penumbra); out += ",\n";
        pad(out, 3); out += "\"pcss_max_penumbra\":          "; emit_float(out, sh.pcss_max_penumbra); out += ",\n";
        pad(out, 3); out += "\"pcss_blocker_search_radius\": "; emit_float(out, sh.pcss_blocker_search_radius); out += "\n";
        pad(out, 2); out += "},\n";

        const auto& ao = g.ssao;
        pad(out, 2); out += "\"ssao\": {\n";
        pad(out, 3); out += "\"sample_count\":  "; emit_uint(out, ao.sample_count); out += ",\n";
        pad(out, 3); out += "\"radius\":        "; emit_float(out, ao.radius); out += ",\n";
        pad(out, 3); out += "\"bias\":          "; emit_float(out, ao.bias); out += ",\n";
        pad(out, 3); out += "\"intensity\":     "; emit_float(out, ao.intensity); out += ",\n";
        pad(out, 3); out += "\"falloff_start\": "; emit_float(out, ao.falloff_start); out += ",\n";
        pad(out, 3); out += "\"falloff_end\":   "; emit_float(out, ao.falloff_end); out += ",\n";
        pad(out, 3); out += "\"blur_enabled\":  "; emit_bool(out, ao.blur_enabled); out += "\n";
        pad(out, 2); out += "},\n";

        const auto& pb = g.probes;
        pad(out, 2); out += "\"probes\": {\n";
        pad(out, 3); out += "\"grid_origin\":        [";
        emit_float(out, pb.grid_origin.x); out += ", ";
        emit_float(out, pb.grid_origin.y); out += ", ";
        emit_float(out, pb.grid_origin.z); out += "],\n";
        pad(out, 3); out += "\"grid_spacing\":       [";
        emit_float(out, pb.grid_spacing.x); out += ", ";
        emit_float(out, pb.grid_spacing.y); out += ", ";
        emit_float(out, pb.grid_spacing.z); out += "],\n";
        pad(out, 3); out += "\"grid_x\":             "; emit_uint(out, pb.grid_x); out += ",\n";
        pad(out, 3); out += "\"grid_y\":             "; emit_uint(out, pb.grid_y); out += ",\n";
        pad(out, 3); out += "\"grid_z\":             "; emit_uint(out, pb.grid_z); out += ",\n";
        pad(out, 3); out += "\"gi_intensity\":       "; emit_float(out, pb.gi_intensity); out += ",\n";
        pad(out, 3); out += "\"max_probe_distance\": "; emit_float(out, pb.max_probe_distance); out += "\n";
        pad(out, 2); out += "}\n";
        out += "  },\n";
    }

    // memory
    {
        const auto& m = def.memory_config;
        out += "  \"memory\": {\n";
        pad(out, 2); out += "\"frame_allocator_size\": "; emit_uint(out, m.frame_allocator_size); out += ",\n";
        pad(out, 2); out += "\"flight_count\":         "; emit_uint(out, m.flight_count); out += ",\n";
        pad(out, 2); out += "\"pool_chunk_size\":      "; emit_uint(out, m.pool_chunk_size); out += ",\n";
        pad(out, 2); out += "\"use_large_pages\":      "; emit_bool(out, m.use_large_pages); out += ",\n";
        const auto& gm = m.gpu_config;
        pad(out, 2); out += "\"gpu\": {\n";
        pad(out, 3); out += "\"mesh_pool_size\":       "; emit_uint(out, gm.mesh_pool_size); out += ",\n";
        pad(out, 3); out += "\"ssbo_pool_size\":       "; emit_uint(out, gm.ssbo_pool_size); out += ",\n";
        pad(out, 3); out += "\"instance_buffer_size\": "; emit_uint(out, gm.instance_buffer_size); out += ",\n";
        pad(out, 3); out += "\"indirect_buffer_size\": "; emit_uint(out, gm.indirect_buffer_size); out += ",\n";
        pad(out, 3); out += "\"staging_buffer_size\":  "; emit_uint(out, gm.staging_buffer_size); out += "\n";
        pad(out, 2); out += "}\n";
        out += "  },\n";
    }

    // gpu_driven
    {
        const auto& g = def.gpu_driven_config;
        out += "  \"gpu_driven\": {\n";
        pad(out, 2); out += "\"max_triangle_count\": "; emit_uint(out, g.max_triangle_count); out += ",\n";
        pad(out, 2); out += "\"min_instance_count\": "; emit_uint(out, g.min_instance_count); out += ",\n";
        pad(out, 2); out += "\"workgroup_size\":     "; emit_uint(out, g.workgroup_size); out += ",\n";
        pad(out, 2); out += "\"two_phase_culling\":  "; emit_bool(out, g.two_phase_culling); out += ",\n";
        pad(out, 2); out += "\"compute_update\":     "; emit_bool(out, g.compute_update); out += "\n";
        out += "  },\n";
    }

    // update
    {
        const auto& u = def.update_config;
        out += "  \"update\": {\n";
        pad(out, 2); out += "\"chunk_size\":         "; emit_uint(out, u.chunk_size); out += ",\n";
        pad(out, 2); out += "\"worker_threads\":     "; emit_uint(out, u.worker_threads); out += ",\n";
        pad(out, 2); out += "\"nt_store_enabled\":   "; emit_bool(out, u.nt_store_enabled); out += ",\n";
        pad(out, 2); out += "\"nt_store_threshold\": "; emit_uint(out, u.nt_store_threshold); out += "\n";
        out += "  },\n";
    }

    // profiler
    {
        const auto& p = def.profiler_config;
        out += "  \"profiler\": {\n";
        pad(out, 2); out += "\"enabled\":      "; emit_bool(out, p.enabled); out += ",\n";
        pad(out, 2); out += "\"overlay_mode\": "; quote(out, overlay_mode_to_str(p.overlay_mode)); out += ",\n";
        pad(out, 2); out += "\"max_queries\":  "; emit_uint(out, p.max_queries); out += "\n";
        out += "  }\n";
    }

    out += "}\n";
    return out;
}

// ============================================================
// Public API — decode
// ============================================================

bool from_pipeline_profile_json(const std::string& json,
                                PipelineProfileDef& out,
                                std::string*        error) {
    out = PipelineProfileDef{};
    return decode(json, out, error);
}

bool from_pipeline_profile_json(const std::string&        json,
                                const PipelineProfileDef& preset,
                                PipelineProfileDef&       out,
                                std::string*              error) {
    out = preset;
    return decode(json, out, error);
}

// ============================================================
// Public API — file I/O
// ============================================================

namespace {

bool read_file(const std::string& path, std::string& contents, std::string* error) {
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) {
        if (error) *error = "could not open file: " + path;
        return false;
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    contents = ss.str();
    // Tolerate a UTF-8 BOM at the head of the file.
    if (contents.size() >= 3 &&
        static_cast<unsigned char>(contents[0]) == 0xEF &&
        static_cast<unsigned char>(contents[1]) == 0xBB &&
        static_cast<unsigned char>(contents[2]) == 0xBF) {
        contents.erase(0, 3);
    }
    return true;
}

} // namespace

bool load_pipeline_profile_file(const std::string&  path,
                                PipelineProfileDef& out,
                                std::string*        error) {
    std::string json;
    if (!read_file(path, json, error)) return false;
    return from_pipeline_profile_json(json, out, error);
}

bool load_pipeline_profile_file(const std::string&        path,
                                const PipelineProfileDef& preset,
                                PipelineProfileDef&       out,
                                std::string*              error) {
    std::string json;
    if (!read_file(path, json, error)) return false;
    return from_pipeline_profile_json(json, preset, out, error);
}

bool save_pipeline_profile_file(const std::string&        path,
                                const PipelineProfileDef& def,
                                std::string*              error) {
    std::ofstream f(path, std::ios::binary);
    if (!f.is_open()) {
        if (error) *error = "could not open file for write: " + path;
        return false;
    }
    f << to_pipeline_profile_json(def);
    if (!f.good()) {
        if (error) *error = "write failed: " + path;
        return false;
    }
    return true;
}

} // namespace pictor
