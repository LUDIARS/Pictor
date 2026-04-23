/// Pictor — text effects smoke test.
///
/// Headless. Builds a 32x32 alpha glyph-like bitmap (a filled square
/// with a hole) in-memory, then runs each post-process effect and each
/// path-data transform, reporting the output dimensions + a sanity
/// check. Exit code 0 on success, 1 on API regression.

#include <cstdio>
#include <cstdint>
#include <cstdlib>

#include "pictor/text/text_effects.h"
#include "pictor/text/glyph_path_effects.h"

using pictor::ImageBuffer;
using pictor::GlyphOutline;
using pictor::SvgPathCommand;
using pictor::SvgPathPoint;

// ─── tiny helpers ────────────────────────────────────────────

static int fails = 0;

#define CHECK(cond, msg) \
    do { if (!(cond)) { std::fprintf(stderr, "FAIL: %s\n", msg); ++fails; } \
         else         { std::printf ("  ok: %s\n", msg); } } while (0)

static ImageBuffer make_fake_glyph_alpha() {
    // 32x32 alpha buffer with an opaque square ring (like a hollow O).
    ImageBuffer b;
    b.allocate(32, 32, 1);
    for (uint32_t y = 0; y < 32; ++y) {
        for (uint32_t x = 0; x < 32; ++x) {
            const bool inside = x >= 4 && x <= 27 && y >= 4 && y <= 27;
            const bool hole   = x >= 10 && x <= 21 && y >= 10 && y <= 21;
            b.pixels[y * 32 + x] = (inside && !hole) ? 255 : 0;
        }
    }
    return b;
}

static GlyphOutline make_fake_glyph_path() {
    // Rectangle path with a secondary rectangle hole, expressed as SVG
    // line-to commands. em_size hints at font-unit scale.
    GlyphOutline g;
    g.codepoint = 'O';
    g.em_size   = 1000.0f;
    g.advance_x = 600.0f;
    auto add = [&](SvgPathCommand c, float x, float y) {
        SvgPathPoint p{};
        p.command = c; p.x = x; p.y = y;
        g.path.push_back(p);
    };
    add(SvgPathCommand::MOVE_TO,  0.0f,   0.0f);
    add(SvgPathCommand::LINE_TO,  500.0f, 0.0f);
    add(SvgPathCommand::LINE_TO,  500.0f, 700.0f);
    add(SvgPathCommand::LINE_TO,  0.0f,   700.0f);
    add(SvgPathCommand::CLOSE,    0.0f,   0.0f);
    return g;
}

// ─── main ────────────────────────────────────────────────────

int main() {
    std::printf("== Pictor text effects smoke test ==\n");

    // Post-process pattern --------------------------------------
    namespace tfx = pictor::text_effects;
    auto alpha = make_fake_glyph_alpha();

    tfx::OutlineParams op; op.width_px = 3; op.color = { 255, 0, 0, 255 };
    auto outlined = tfx::apply_outline(alpha, op);
    CHECK(outlined.channels == 4,                     "outline: RGBA output");
    CHECK(outlined.width  == alpha.width  + 2 * op.width_px,
          "outline: output wider by 2*width");
    CHECK(outlined.height == alpha.height + 2 * op.width_px,
          "outline: output taller by 2*width");

    tfx::DropShadowParams sp; sp.offset_x_px = 3; sp.offset_y_px = 3;
    sp.blur_radius = 2; sp.color = { 0, 0, 0, 200 };
    auto shadow = tfx::apply_drop_shadow(alpha, sp);
    CHECK(shadow.channels == 4,  "drop_shadow: RGBA output");
    CHECK(shadow.width  >= alpha.width,  "drop_shadow: not narrower than src");
    CHECK(shadow.height >= alpha.height, "drop_shadow: not shorter than src");

    tfx::GlowParams gp; gp.radius_px = 6; gp.intensity = 1.3f;
    gp.color = { 200, 220, 255, 255 };
    auto glow = tfx::apply_glow(alpha, gp);
    CHECK(glow.channels == 4,  "glow: RGBA output");
    CHECK(glow.width  == alpha.width  + 2 * gp.radius_px,
          "glow: output padded by 2*radius");

    // Path-data pattern -----------------------------------------
    namespace gfx = pictor::glyph_path_effects;
    auto path = make_fake_glyph_path();

    gfx::OutlineParams pop; pop.width = 30.0f; pop.segments_per_curve = 4;
    auto offset = gfx::offset_outline(path, pop);
    CHECK(!offset.path.empty(), "offset_outline: emits new path");

    gfx::DropShadowParams dsp; dsp.dx = 20.0f; dsp.dy = 25.0f;
    auto translated = gfx::translate_path(path, dsp);
    CHECK(translated.path.size() == path.path.size(),
          "translate_path: same vertex count");
    // The first MOVE_TO should be shifted by (dx, dy).
    CHECK(translated.path[0].x == path.path[0].x + dsp.dx,
          "translate_path: x shifted");
    CHECK(translated.path[0].y == path.path[0].y + dsp.dy,
          "translate_path: y shifted");

    gfx::GlowParams glp;   // default 3 steps
    auto glow_layers = gfx::scale_outward(path, glp);
    CHECK(glow_layers.size() == glp.steps.size(),
          "scale_outward: one layer per step");

    // Flatten works too.
    auto flat = gfx::flatten_to_polyline(path, 4);
    CHECK(!flat.path.empty(), "flatten: non-empty result");

    std::printf("\nresult: %s (%d failure%s)\n",
                fails == 0 ? "ALL PASS" : "FAILED",
                fails, fails == 1 ? "" : "s");
    return fails == 0 ? 0 : 1;
}
