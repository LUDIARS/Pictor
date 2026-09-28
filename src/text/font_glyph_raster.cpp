// @spec SPEC-PC-VECTOR-TEXT (spec/feature/subsystem/text.md)
#include "font_glyph_raster.h"
#include "pictor/text/text_svg_renderer.h"
#include "pictor/text/glyph_path_effects.h"
#include "pictor/text/glyph_vector_rasterizer.h"
#include <cmath>
#include <stdexcept>

namespace pictor::detail {
ImageBuffer rasterize_font_glyph(const FontLoader& fonts, FontHandle font,
                                uint32_t codepoint, float size, GlyphMetrics& metrics) {
    if (!std::isfinite(size) || size<=0) throw std::invalid_argument("Invalid font size");
    if (!fonts.get_glyph_metrics(font,codepoint,size,metrics)) return {};
    const auto outline=TextSvgRenderer(fonts).extract_glyph_outline(font,codepoint);
    if (outline.path.empty()) { metrics.width=metrics.height=0; return {}; }
    if (outline.em_size<=0) throw std::invalid_argument("Invalid font em size");
    const auto bounds=glyph_path_effects::compute_bbox(outline);
    const float scale=size/outline.em_size;
    const float left=std::floor(bounds.min_x*scale), right=std::ceil(bounds.max_x*scale);
    const float top=std::ceil(bounds.max_y*scale), bottom=std::floor(bounds.min_y*scale);
    if (!bounds.valid || !std::isfinite(left) || !std::isfinite(right) || !std::isfinite(top) || !std::isfinite(bottom) ||
        right-left>4096 || top-bottom>4096 || right<left || top<bottom)
        throw std::invalid_argument("Invalid glyph raster bounds");
    if (right==left || top==bottom) { metrics.width=metrics.height=0; return {}; }
    // The image origin and bearings are paired: neither clip overhangs nor add
    // the left side bearing twice when the caller places this image.
    metrics.bearing_x=left; metrics.bearing_y=top;
    metrics.width=right-left; metrics.height=top-bottom;
    return GlyphVectorRasterizer().render(outline,static_cast<uint32_t>(metrics.width),
        static_cast<uint32_t>(metrics.height),scale,-left,top);
}
}
