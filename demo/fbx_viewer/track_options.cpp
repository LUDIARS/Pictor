#include "track_options.h"

#include "pictor/core/float_parse.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace pictor_fbx_viewer {

namespace {

/// Parses exactly `count` comma-separated finite numbers.
bool parse_float_list(const char* text, float* out, int count) {
    const char* p = text;
    const char* end = text + std::strlen(text);
    for (int k = 0; k < count; ++k) {
        double v = 0.0;
        const auto r = pictor::float_parse::parse_double(p, end, v);
        if (r.ec != std::errc() || !std::isfinite(v)) return false;
        out[k] = static_cast<float>(v);
        p = r.ptr;
        if (k + 1 < count) {
            if (p == end || *p != ',') return false;
            ++p;
        }
    }
    return p == end;
}

bool parse_uint(const char* text, uint32_t min_value, uint32_t max_value, uint32_t& out) {
    if (!text || *text < '0' || *text > '9') return false;
    char* end = nullptr;
    const unsigned long v = std::strtoul(text, &end, 10);
    if (!end || *end != '\0' || v < min_value || v > max_value) return false;
    out = static_cast<uint32_t>(v);
    return true;
}

bool parse_size(const char* text, uint32_t& w, uint32_t& h) {
    const char* x = std::strchr(text, 'x');
    if (!x) return false;
    const std::string ws(text, x);
    return parse_uint(ws.c_str(), 16, 8192, w) && parse_uint(x + 1, 16, 8192, h);
}

} // namespace

/// @implements SPEC-PC-FBX-TRACK-PLAYBACK
OptionParse parse_track_option(int argc, char** argv, int& i, TrackOptions& out) {
    const char* a = argv[i];
    auto value = [&]() -> const char* { return i + 1 < argc ? argv[++i] : nullptr; };
    auto fail = [&](const char* msg) { std::fprintf(stderr, "%s\n", msg); return OptionParse::Error; };

    if (std::strcmp(a, "--list-channels") == 0) { out.list_channels = true; return OptionParse::Ok; }
    if (std::strcmp(a, "--track") == 0) {
        const char* v = value();
        if (!v || !*v) return fail("--track needs a CSV file");
        out.track_path = v;
        return OptionParse::Ok;
    }
    if (std::strcmp(a, "--track-fps") == 0) {
        if (!parse_uint(value(), 1, 120, out.track_fps)) return fail("--track-fps must be an integer from 1 to 120");
        return OptionParse::Ok;
    }
    if (std::strcmp(a, "--raw-out") == 0) {
        const char* v = value();
        if (!v || !*v) return fail("--raw-out needs a file path or -");
        out.raw_out = v;
        return OptionParse::Ok;
    }
    if (std::strcmp(a, "--size") == 0) {
        const char* v = value();
        if (!v || !parse_size(v, out.width, out.height)) return fail("--size must be <W>x<H> (16..8192)");
        out.has_size = true;
        return OptionParse::Ok;
    }
    if (std::strcmp(a, "--camera") == 0) {
        const char* v = value();
        float f[6];
        if (!v || !parse_float_list(v, f, 6)) return fail("--camera must be ex,ey,ez,tx,ty,tz");
        for (int k = 0; k < 3; ++k) { out.eye[k] = f[k]; out.target[k] = f[k + 3]; }
        out.has_camera = true;
        return OptionParse::Ok;
    }
    if (std::strcmp(a, "--fov") == 0) {
        const char* v = value();
        if (!v || !parse_float_list(v, &out.fov_deg, 1) || out.fov_deg <= 1.0f || out.fov_deg >= 179.0f)
            return fail("--fov must be a vertical angle in degrees (1..179)");
        return OptionParse::Ok;
    }
    if (std::strcmp(a, "--clear") == 0) {
        const char* v = value();
        if (!v || !parse_float_list(v, out.clear, 3)) return fail("--clear must be r,g,b");
        for (float c : out.clear) if (c < 0.0f || c > 1.0f) return fail("--clear components must be 0..1");
        out.has_clear = true;
        return OptionParse::Ok;
    }
    return OptionParse::NotMine;
}

/// @implements SPEC-PC-FBX-TRACK-PLAYBACK
bool validate_track_options(const TrackOptions& opts) {
    if (!opts.raw_out.empty() && !opts.track_enabled()) {
        std::fprintf(stderr, "--raw-out requires --track\n");
        return false;
    }
    if (opts.list_channels && (opts.track_enabled() || !opts.raw_out.empty())) {
        std::fprintf(stderr, "--list-channels cannot be combined with --track or --raw-out\n");
        return false;
    }
    if (opts.has_camera) {
        const float d[3] = {opts.target[0] - opts.eye[0], opts.target[1] - opts.eye[1], opts.target[2] - opts.eye[2]};
        if (d[0] * d[0] + d[1] * d[1] + d[2] * d[2] < 1e-12f) {
            std::fprintf(stderr, "--camera eye and target must differ\n");
            return false;
        }
    }
    return true;
}

bool wants_clean_stdout(int argc, char** argv, bool& binary) {
    binary = false;
    bool clean = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--list-channels") == 0) clean = true;
        if (std::strcmp(argv[i], "--raw-out") == 0 && i + 1 < argc && std::strcmp(argv[i + 1], "-") == 0) {
            clean = true;
            binary = true;
        }
    }
    return clean;
}

} // namespace pictor_fbx_viewer
