#include "pictor/vector_text/glyph_outline_source.h"

#include "pictor/text/font_loader.h"
#include "pictor/text/text_svg_renderer.h"

namespace pictor {

GlyphOutline FontOutlineSource::outline(FontHandle font, uint32_t codepoint) const {
    const TextSvgRenderer reader(fonts_);
    GlyphOutline result = reader.extract_glyph_outline(font, codepoint);
    if (const FontMetrics* metrics = fonts_.get_metrics(font)) {
        if (!(result.em_size > 0.0f)) result.em_size = static_cast<float>(metrics->units_per_em);
    }
    return result;
}

float FontOutlineSource::units_per_em(FontHandle font) const {
    const FontMetrics* metrics = fonts_.get_metrics(font);
    return metrics != nullptr ? static_cast<float>(metrics->units_per_em) : 0.0f;
}

float FontOutlineSource::kerning(FontHandle font, uint32_t left, uint32_t right) const {
    return static_cast<float>(fonts_.get_kerning(font, left, right));
}

float FontOutlineSource::line_height(FontHandle font) const {
    const FontMetrics* metrics = fonts_.get_metrics(font);
    return metrics != nullptr ? metrics->line_height : 0.0f;
}

}  // namespace pictor
