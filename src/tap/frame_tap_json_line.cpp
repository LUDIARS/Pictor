#include "pictor/tap/frame_tap_json_line.h"

#include <charconv>
#include <cmath>
#include <cstdio>
#include <system_error>
#ifdef __ANDROID__
#include <locale>
#include <sstream>
#endif

namespace pictor {

namespace {

// 数値の書き方は visus_json.cpp の emit_number と同じ方針 (locale 非依存、
// 古い NDK の libc++ は浮動小数点 to_chars を持たないので classic locale の stream)。
// ただしタップは float を最短表現で出す (1 行を短く保つ)。 有限でない値は
// 偽の 0 ではなく null にする。
void append_float(std::string& out, float v) {
    if (!std::isfinite(v)) { out += "null"; return; }
#ifdef __ANDROID__
    std::ostringstream number;
    number.imbue(std::locale::classic());
    number.precision(9);
    number << v;
    out += number.str();
#else
    char buf[32];
    const auto result = std::to_chars(buf, buf + sizeof(buf), v);
    if (result.ec != std::errc{}) { out += "null"; return; }
    out.append(buf, static_cast<size_t>(result.ptr - buf));
#endif
}

void append_double(std::string& out, double v) {
    if (!std::isfinite(v)) { out += "null"; return; }
#ifdef __ANDROID__
    std::ostringstream number;
    number.imbue(std::locale::classic());
    number.precision(17);
    number << v;
    out += number.str();
#else
    char buf[32];
    const auto result = std::to_chars(buf, buf + sizeof(buf), v);
    if (result.ec != std::errc{}) { out += "null"; return; }
    out.append(buf, static_cast<size_t>(result.ptr - buf));
#endif
}

void append_uint(std::string& out, uint64_t v) {
    char buf[24];
    const auto result = std::to_chars(buf, buf + sizeof(buf), v);
    out.append(buf, static_cast<size_t>(result.ptr - buf));
}

void append_quoted(std::string& out, std::string_view s) {
    out.push_back('"');
    for (const char c : s) {
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

/// m[row][col] を行優先で 16 個 (Pictor の行ベクトル規約のまま)。
void append_matrix(std::string& out, const float4x4& m) {
    out.push_back('[');
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            if (r != 0 || c != 0) out.push_back(',');
            append_float(out, m.m[r][c]);
        }
    }
    out.push_back(']');
}

void append_string_array(std::string& out, const std::vector<std::string>& values) {
    out.push_back('[');
    for (size_t i = 0; i < values.size(); ++i) {
        if (i != 0) out.push_back(',');
        append_quoted(out, values[i]);
    }
    out.push_back(']');
}

void append_draw(std::string& out, const FrameTapDraw& draw) {
    out += "{\"mesh\":";
    append_quoted(out, draw.mesh);
    out += ",\"material\":";
    append_string_array(out, draw.materials);
    out += ",\"instance\":";
    append_uint(out, draw.instance);
    out += ",\"world\":";
    append_matrix(out, draw.world);
    out += ",\"screen_bbox\":[";
    append_float(out, draw.screen_bbox.x);
    out.push_back(',');
    append_float(out, draw.screen_bbox.y);
    out.push_back(',');
    append_float(out, draw.screen_bbox.w);
    out.push_back(',');
    append_float(out, draw.screen_bbox.h);
    out += "],\"depth_order\":";
    append_uint(out, draw.depth_order);
    out += ",\"tags\":";
    append_string_array(out, draw.tags);
    out.push_back('}');
}

void append_pass(std::string& out, const FrameTapFrame& frame, FrameTapPass pass) {
    out += "{\"pass\":";
    append_quoted(out, frame_tap_pass_name(pass));
    out += ",\"draws\":[";
    bool first = true;
    for (size_t i = 0; i < frame.draw_count; ++i) {
        const FrameTapDraw& draw = frame.draws[i];
        if (draw.pass != pass) continue;
        if (!first) out.push_back(',');
        append_draw(out, draw);
        first = false;
    }
    out += "]}";
}

} // namespace

void encode_frame_tap_line(const FrameTapFrame& frame, std::string& out) {
    out.clear();
    out += "{\"frame\":";
    append_uint(out, frame.frame);
    out += ",\"t\":";
    append_double(out, frame.t);
    out += ",\"camera\":{\"view\":";
    append_matrix(out, frame.camera.view);
    out += ",\"projection\":";
    append_matrix(out, frame.camera.projection);
    out += ",\"viewport\":[";
    append_uint(out, frame.camera.viewport_width);
    out.push_back(',');
    append_uint(out, frame.camera.viewport_height);
    out += "]},\"passes\":[";
    append_pass(out, frame, FrameTapPass::SCENE);
    out.push_back(',');
    append_pass(out, frame, FrameTapPass::UI);
    out += "]}";
}

} // namespace pictor
