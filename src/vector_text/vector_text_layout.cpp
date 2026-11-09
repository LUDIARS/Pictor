#include "pictor/vector_text/vector_text_layout.h"

#include "pictor/vector_text/size_tier.h"

#include <algorithm>
#include <cmath>

namespace pictor {

uint32_t next_utf8_codepoint(std::string_view s, size_t& at) {
    constexpr uint32_t kReplacement = 0xFFFD;
    const auto byte = [&](size_t i) { return static_cast<uint8_t>(s[i]); };
    const uint8_t lead = byte(at++);
    if (lead < 0x80) return lead;
    uint32_t count = 0, cp = 0, min = 0;
    if ((lead & 0xE0) == 0xC0) {
        count = 1, cp = lead & 0x1FU, min = 0x80;
    } else if ((lead & 0xF0) == 0xE0) {
        count = 2, cp = lead & 0x0FU, min = 0x800;
    } else if ((lead & 0xF8) == 0xF0) {
        count = 3, cp = lead & 0x07U, min = 0x10000;
    } else {
        return kReplacement;
    }
    for (uint32_t i = 0; i < count; ++i) {
        if (at >= s.size() || (byte(at) & 0xC0) != 0x80) return kReplacement;  // the bad byte is read next
        cp = (cp << 6) | (byte(at++) & 0x3FU);
    }
    if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return kReplacement;
    return cp;
}

namespace {

/// Walks the text glyph by glyph; `place` gets (mesh, pen x in px, line index).
template <typename Place>
float walk(GlyphMeshCache& cache, FontHandle font, std::string_view utf8, float pixels_per_em, float spacing_px,
           Place&& place) {
    const IGlyphOutlineSource& source = cache.source();
    const float units = source.units_per_em(font);
    const float kern_scale = units > 0.0f ? pixels_per_em / units : 0.0f;
    const uint32_t tier = glyph_size_tier(pixels_per_em);
    float pen = 0.0f, widest = 0.0f;
    uint32_t line = 0, previous = 0;
    size_t at = 0;
    while (at < utf8.size()) {
        const uint32_t cp = next_utf8_codepoint(utf8, at);
        if (cp == '\n') {
            widest = std::max(widest, pen);
            pen = 0.0f;
            previous = 0;
            ++line;
            continue;
        }
        if (previous != 0) pen += source.kerning(font, previous, cp) * kern_scale;
        const uint32_t mesh = cache.mesh_for(font, cp, tier);
        place(mesh, pen, line);
        pen += cache.range(mesh).advance_em * pixels_per_em + spacing_px;
        previous = cp;
    }
    return std::max(widest, pen);
}

}  // namespace

float layout_vector_text(GlyphMeshCache& cache, FontHandle font, std::string_view utf8, const VectorTextStyle& style,
                         float origin_x, float baseline_y, VectorTextBatch& batch) {
    const IGlyphOutlineSource& source = cache.source();
    const float units = source.units_per_em(font);
    const float line_px = units > 0.0f ? source.line_height(font) / units * style.pixels_per_em : 0.0f;
    // Screen y points down, em y points up; rotation turns clockwise on screen.
    const float c = std::cos(style.rotation_rad), s = std::sin(style.rotation_rad);
    const float k = style.pixels_per_em;
    VectorTextInstance base;
    base.m[0] = c * k;   // em x axis -> screen
    base.m[1] = s * k;
    base.m[2] = s * k;   // em y axis (up) -> screen
    base.m[3] = -c * k;
    base.feather_px = style.feather_px;
    base.embolden_px = style.embolden_px;
    std::copy(std::begin(style.color), std::end(style.color), std::begin(base.color));
    return walk(cache, font, utf8, style.pixels_per_em, style.letter_spacing_px, [&](uint32_t mesh, float pen, uint32_t line) {
        if (cache.range(mesh).index_count == 0) return;  // blank
        // Pen moves along the rotated baseline; lines step along the rotated down axis.
        const float down = static_cast<float>(line) * line_px;
        VectorTextInstance instance = base;
        instance.tx = origin_x + c * pen - s * down;
        instance.ty = baseline_y + s * pen + c * down;
        batch.add(mesh, instance);
    });
}

float measure_vector_text(GlyphMeshCache& cache, FontHandle font, std::string_view utf8, float pixels_per_em,
                          float letter_spacing_px) {
    return walk(cache, font, utf8, pixels_per_em, letter_spacing_px, [](uint32_t, float, uint32_t) {});
}

}  // namespace pictor
