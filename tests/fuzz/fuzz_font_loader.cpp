// Fuzz entry: FontLoader::load_from_memory (table directory, hmtx, cmap, kern, name)
// and TrueType outline decoding of every mapped glyph.
#include "fuzz_entry.h"
#include "synthetic_font.h"

#include "pictor/text/font_loader.h"
#include "pictor/text/text_svg_renderer.h"

#include <stdexcept>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    pictor::FontLoader loader;
    const pictor::FontHandle font = loader.load_from_memory(data, size, "fuzz");
    if (font == pictor::INVALID_FONT) return 0;
    pictor::GlyphMetrics metrics;
    pictor::TextSvgRenderer svg(loader);
    for (uint32_t codepoint : {0x20u, 0x41u, 0x3042u, 0x1F600u}) {
        (void)loader.get_glyph_metrics(font, codepoint, 16.0f, metrics);
        (void)loader.get_kerning(font, codepoint, 0x41u);
        try {
            (void)svg.extract_glyph_outline(font, codepoint);
        } catch (const std::invalid_argument&) {
            // Documented rejection of invalid outline data.
        }
    }
    return 0;
}

std::vector<std::vector<uint8_t>> pictor_fuzz_seeds() {
    return {pictor_test::make_synthetic_font(),
            pictor_test::make_synthetic_font({4, 1})};
}
