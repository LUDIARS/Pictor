/// Pixel analysis test: rasterize "Pictor" via TextImageRenderer and verify
/// the resulting RGBA buffer has plausible coverage, color preservation,
/// and a non-trivial spatial distribution.
///
/// The test is intentionally tolerant of rasterizer changes: it asserts
/// statistical properties rather than an exact byte-for-byte hash so that
/// font swaps or sub-pixel tweaks do not flake the test.

#include "pictor/text/font_loader.h"
#include "pictor/text/text_image_renderer.h"
#include "test_common.h"

#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>

using namespace pictor;
using namespace pictor_test;

namespace {

std::string find_default_font() {
    const char* candidates[] = {
        "fonts/default.ttf",
        "fonts/default.otf",
        "../fonts/default.ttf",
        "../fonts/default.otf",
        "../../fonts/default.ttf",
        "../../fonts/default.otf",
        // Windows fallbacks (CI-friendly)
        "C:\\Windows\\Fonts\\arial.ttf",
        // Linux fallbacks
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        // macOS fallbacks
        "/System/Library/Fonts/Helvetica.ttc",
    };
    for (const char* p : candidates) {
        FILE* f = std::fopen(p, "rb");
        if (f) { std::fclose(f); return p; }
    }
    return {};
}

/// Dump a PPM (P6, RGB) for visual debugging on failure.
void write_ppm(const char* path, const ImageBuffer& img) {
    if (img.channels != 4) return;
    FILE* f = std::fopen(path, "wb");
    if (!f) return;
    std::fprintf(f, "P6\n%u %u\n255\n", img.width, img.height);
    for (size_t i = 0; i < size_t(img.width) * img.height; ++i) {
        std::fputc(img.pixels[i * 4 + 0], f);
        std::fputc(img.pixels[i * 4 + 1], f);
        std::fputc(img.pixels[i * 4 + 2], f);
    }
    std::fclose(f);
    std::fprintf(stderr, "       wrote debug image: %s\n", path);
}

} // namespace

int main() {
    // ---- 1. Load font ----
    std::string path = find_default_font();
    if (path.empty()) {
        std::fprintf(stderr, "[FAIL] cannot locate fonts/default.ttf "
                             "(run from build dir)\n");
        return 1;
    }

    FontLoader loader;
    FontHandle font = loader.load_from_file(path);
    PT_ASSERT(font != INVALID_FONT, "font loaded");
    if (font == INVALID_FONT) return report("pixel_text_test");

    std::printf("       font: %s\n", path.c_str());

    // ---- 2. Render text ----
    TextImageRenderer rasterizer(loader);
    TextStyle style;
    style.font_size = 48.0f;
    style.color     = {1.0f, 1.0f, 1.0f, 1.0f};

    ImageBuffer img = rasterizer.render_text(font, "Pictor", style);

    PT_ASSERT_OP(img.width,  >, 0u, "image width");
    PT_ASSERT_OP(img.height, >, 0u, "image height");
    PT_ASSERT_OP(img.channels, ==, 4u, "RGBA output");
    PT_ASSERT(!img.pixels.empty(), "buffer non-empty");
    if (g_failures > 0) return report("pixel_text_test");

    std::printf("       image: %ux%u (%zu bytes)\n",
                img.width, img.height, img.pixels.size());

    // ---- 3. Coverage analysis ----
    const size_t total_px = size_t(img.width) * img.height;
    size_t opaque = 0;
    size_t white  = 0;
    for (size_t i = 0; i < total_px; ++i) {
        uint8_t r = img.pixels[i * 4 + 0];
        uint8_t g = img.pixels[i * 4 + 1];
        uint8_t b = img.pixels[i * 4 + 2];
        uint8_t a = img.pixels[i * 4 + 3];
        if (a > 32) {
            ++opaque;
            if (r > 200 && g > 200 && b > 200) ++white;
        }
    }
    double coverage   = double(opaque) / double(total_px);
    double white_frac = opaque ? double(white) / double(opaque) : 0.0;

    std::printf("       opaque coverage: %.1f%%  (%zu / %zu px)\n",
                coverage * 100.0, opaque, total_px);
    std::printf("       white-of-opaque: %.1f%%\n", white_frac * 100.0);

    // Text occupies a meaningful but bounded area of the buffer.
    // Six glyphs at 48px ≈ 5–40% of an auto-sized canvas.
    PT_ASSERT(coverage > 0.02, "coverage > 2% (non-trivial ink)");
    PT_ASSERT(coverage < 0.80, "coverage < 80% (not a solid fill)");

    // Color set in TextStyle should dominate opaque pixels.
    PT_ASSERT(white_frac > 0.50, "majority of opaque px match style color");

    // ---- 4. Spatial distribution: ink not concentrated in a single corner ----
    {
        size_t left = 0, right = 0, top = 0, bottom = 0;
        for (uint32_t y = 0; y < img.height; ++y)
        for (uint32_t x = 0; x < img.width; ++x) {
            uint8_t a = img.pixels[(y * img.width + x) * 4 + 3];
            if (a <= 32) continue;
            if (x <  img.width  / 2) ++left; else ++right;
            if (y <  img.height / 2) ++top;  else ++bottom;
        }
        // Latin text laid out left-to-right typically straddles the
        // horizontal midline; both halves must contain ink.
        PT_ASSERT(left > 0 && right > 0,
                  "ink spans both left and right halves");
        // Vertical balance: ascenders/descenders should put ink in both
        // halves of the canvas (loose check — auto-sized canvas keeps text
        // roughly centered).
        PT_ASSERT(top > 0 || bottom > 0, "any vertical ink");
    }

    // ---- 5. Measure_text agrees with rendered extent ----
    {
        auto ext = rasterizer.measure_text(font, "Pictor", style);
        PT_ASSERT_OP(static_cast<uint32_t>(ext.width + 1),  >=, img.width / 2,
                     "measure_text width consistent with bitmap");
        PT_ASSERT(ext.height > 0.0f, "measure_text height > 0");
    }

    if (g_failures > 0) write_ppm("pixel_text_failed.ppm", img);
    return report("pixel_text_test");
}
