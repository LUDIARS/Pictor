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
// ただしタップは float を最短表現で出す (1 行を短く保つ)。 契約は数値に null を
// 許さないので、 有限でない値は FrameTap が draw ごと dropped に回してからここへ来る。
// それでも届いた場合 (直接呼び出し) は偽の 0 ではなく null を出し、 schema 違反として
// 受信側に見えるようにする。
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

/// m[row][col] を行優先で 16 個 (Pictor の行ベクトル規約のまま)。 行ベクトル規約の
/// 行優先の並びは、 列ベクトル規約の同じ変換を列優先に並べたものと同じ 16 個になる
/// (平行移動は 12〜14 番目) — 契約の「列優先・列ベクトル」と値を変えずに一致する。
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

void append_rect(std::string& out, const FrameTapScreenRect& rect) {
    out.push_back('[');
    append_float(out, rect.x);
    out.push_back(',');
    append_float(out, rect.y);
    out.push_back(',');
    append_float(out, rect.w);
    out.push_back(',');
    append_float(out, rect.h);
    out.push_back(']');
}

void append_ui(std::string& out, const FrameTapDraw& draw) {
    out += ",\"ui\":{\"role\":";
    append_quoted(out, frame_tap_ui_role_name(draw.ui_role));
    if (!draw.ui_element.empty()) {
        out += ",\"element\":";
        append_quoted(out, draw.ui_element);
    }
    if (draw.ui_role == FrameTapUiRole::BAR) {
        out += ",\"fill\":";
        append_float(out, draw.ui_fill);
    } else {
        out += ",\"glyph\":";
        append_quoted(out, draw.ui_glyph);
    }
    out.push_back('}');
}

void append_draw(std::string& out, const FrameTapDraw& draw) {
    out += "{\"mesh\":";
    append_quoted(out, draw.mesh);
    out += ",\"material\":";
    append_string_array(out, draw.materials);
    out += ",\"identity\":";
    append_quoted(out, frame_tap_identity_name(draw.identity));
    out += ",\"instance\":";
    append_uint(out, draw.instance);
    out += ",\"generation\":";
    append_uint(out, draw.generation);
    out += ",\"world\":";
    append_matrix(out, draw.world);
    out += ",\"screen_bbox\":";
    append_rect(out, draw.screen_bbox);
    out += ",\"depth_order\":";
    append_uint(out, draw.depth_order);
    out += ",\"visibility\":";
    append_quoted(out, frame_tap_visibility_name(draw.visibility));
    if (draw.has_alpha) {
        out += ",\"alpha\":";
        append_float(out, draw.alpha);
    }
    if (draw.has_clip) {
        out += ",\"clip\":";
        append_rect(out, draw.clip);
    }
    if (draw.ui_role != FrameTapUiRole::NONE) append_ui(out, draw);
    out += ",\"tags\":";
    append_string_array(out, draw.tags);
    out.push_back('}');
}

void append_pass(std::string& out, const FrameTapFrame& frame, uint32_t pass) {
    const FrameTapPassInfo& info = frame.passes[pass];
    out += "{\"name\":";
    append_quoted(out, info.name);
    out += ",\"kind\":";
    append_quoted(out, frame_tap_pass_kind_name(info.kind));
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

void append_header(std::string& out, uint64_t seq) {
    out += "{\"contract\":";
    append_quoted(out, kFrameTapContract);
    out += ",\"seq\":";
    append_uint(out, seq);
}

} // namespace

void encode_frame_tap_line(const FrameTapFrame& frame, std::string& out) {
    out.clear();
    append_header(out, frame.seq);
    out += ",\"frame\":";
    append_uint(out, frame.frame);
    if (frame.has_tick) {
        out += ",\"tick\":";
        append_uint(out, frame.tick);
    }
    out += ",\"t\":";
    append_double(out, frame.t);
    out += ",\"observer\":{\"id\":";
    append_quoted(out, frame.observer);
    out += ",\"viewport\":[";
    append_uint(out, frame.camera.viewport_width);
    out.push_back(',');
    append_uint(out, frame.camera.viewport_height);
    out += "]},\"camera\":{\"view\":";
    append_matrix(out, frame.camera.view);
    out += ",\"projection\":";
    append_matrix(out, frame.camera.projection);
    out.push_back('}');
    if (frame.has_visibility_lag) {
        out += ",\"visibility_lag_frames\":";
        append_uint(out, frame.visibility_lag_frames);
    }
    out += ",\"passes\":[";
    for (uint32_t pass = 0; pass < frame.pass_count; ++pass) {
        if (pass != 0) out.push_back(',');
        append_pass(out, frame, pass);
    }
    out.push_back(']');
    if (frame.dropped_draws != 0) {
        out += ",\"dropped\":{\"draws\":";
        append_uint(out, frame.dropped_draws);
        out += ",\"reason\":";
        append_quoted(out, frame_tap_drop_reason_name(frame.dropped_reason));
        out.push_back('}');
    }
    out.push_back('}');
}

void encode_frame_tap_end_line(uint64_t seq, FrameTapEndReason reason, std::string& out) {
    out.clear();
    append_header(out, seq);
    out += ",\"end\":";
    append_quoted(out, frame_tap_end_reason_name(reason));
    out.push_back('}');
}

} // namespace pictor
