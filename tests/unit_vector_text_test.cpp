/// Pictor::vector_text — outlines become cached triangle meshes that fill
/// exactly the nonzero interior, with a feather band only on the outer edge.
/// Uses synthetic outlines, so no font file is needed.

#include "pictor/vector_text/glyph_contours.h"
#include "pictor/vector_text/glyph_mesh_builder.h"
#include "pictor/vector_text/glyph_mesh_cache.h"
#include "pictor/vector_text/nonzero_tessellator.h"
#include "pictor/vector_text/size_tier.h"
#include "pictor/vector_text/vector_text_layout.h"
#include "test_common.h"

#include <cmath>
#include <stdexcept>
#include <string>

using namespace pictor;
using namespace pictor_test;

namespace {

constexpr float kEm = 1000.0f;

void add_rect(GlyphOutline& o, float x0, float y0, float x1, float y1, bool counter_clockwise) {
    auto point = [](SvgPathCommand c, float x, float y) {
        SvgPathPoint p;
        p.command = c;
        p.x = x;
        p.y = y;
        return p;
    };
    o.path.push_back(point(SvgPathCommand::MOVE_TO, x0, y0));
    if (counter_clockwise) {
        o.path.push_back(point(SvgPathCommand::LINE_TO, x1, y0));
        o.path.push_back(point(SvgPathCommand::LINE_TO, x1, y1));
        o.path.push_back(point(SvgPathCommand::LINE_TO, x0, y1));
    } else {
        o.path.push_back(point(SvgPathCommand::LINE_TO, x0, y1));
        o.path.push_back(point(SvgPathCommand::LINE_TO, x1, y1));
        o.path.push_back(point(SvgPathCommand::LINE_TO, x1, y0));
    }
    o.path.push_back(point(SvgPathCommand::CLOSE, 0, 0));
}

GlyphOutline outline_of(float advance) {
    GlyphOutline o;
    o.em_size   = kEm;
    o.advance_x = advance;
    return o;
}

/// A circle of radius r around (cx, cy) from four quadratic arcs.
void add_circle(GlyphOutline& o, float cx, float cy, float r) {
    const float k = r * std::tan(3.14159265f / 8.0f);  // eight 45-degree quads
    SvgPathPoint m;
    m.command = SvgPathCommand::MOVE_TO;
    m.x = cx + r;
    m.y = cy;
    o.path.push_back(m);
    for (int i = 0; i < 8; ++i) {
        const float a0 = 3.14159265f / 4.0f * static_cast<float>(i);
        const float a1 = a0 + 3.14159265f / 4.0f, am = 0.5f * (a0 + a1);
        const float rc = std::sqrt(r * r + k * k);
        SvgPathPoint q;
        q.command = SvgPathCommand::QUAD_TO;
        q.cx = cx + rc * std::cos(am);
        q.cy = cy + rc * std::sin(am);
        q.x = cx + r * std::cos(a1);
        q.y = cy + r * std::sin(a1);
        o.path.push_back(q);
    }
    SvgPathPoint z;
    z.command = SvgPathCommand::CLOSE;
    o.path.push_back(z);
}

double fill_area(const GlyphMeshData& mesh) {
    double area = 0.0;
    for (size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
        const GlyphVertex& a = mesh.vertices[mesh.indices[i]];
        const GlyphVertex& b = mesh.vertices[mesh.indices[i + 1]];
        const GlyphVertex& c = mesh.vertices[mesh.indices[i + 2]];
        if (a.coverage < 1.0f || b.coverage < 1.0f || c.coverage < 1.0f) continue;
        if (a.offset_x != 0 || a.offset_y != 0 || b.offset_x != 0 || b.offset_y != 0 || c.offset_x != 0 ||
            c.offset_y != 0) {
            continue;  // feather triangles
        }
        area += std::fabs((b.x - a.x) * (c.y - a.y) - (c.x - a.x) * (b.y - a.y)) * 0.5;
    }
    return area;
}

size_t feather_vertex_count(const GlyphMeshData& mesh) {
    size_t n = 0;
    for (const GlyphVertex& v : mesh.vertices) n += (v.offset_x != 0.0f || v.offset_y != 0.0f) ? 1 : 0;
    return n;
}

bool near(double a, double b, double eps) { return std::fabs(a - b) <= eps; }

GlyphMeshData fill_only(const GlyphOutline& o) {
    GlyphMeshData mesh;
    tessellate_nonzero(flatten_glyph_contours(o, glyph_tier_tolerance_em(5)), mesh);
    return mesh;
}

void square_fills_its_area() {
    GlyphOutline o = outline_of(600);
    add_rect(o, 100, 0, 500, 400, true);
    const GlyphMeshData mesh = fill_only(o);
    PT_ASSERT(near(fill_area(mesh), 0.16, 1e-6), "a 0.4 em square covers 0.16 em^2");
}

void hole_with_opposite_winding_is_cut_out() {
    GlyphOutline o = outline_of(600);
    add_rect(o, 0, 0, 600, 600, true);
    add_rect(o, 200, 200, 400, 400, false);
    PT_ASSERT(near(fill_area(fill_only(o)), 0.36 - 0.04, 1e-6), "the hole is not filled");
}

void same_winding_overlap_is_a_union() {
    GlyphOutline o = outline_of(600);
    add_rect(o, 0, 0, 400, 400, true);
    add_rect(o, 200, 200, 600, 600, true);
    PT_ASSERT(near(fill_area(fill_only(o)), 0.16 + 0.16 - 0.04, 1e-6), "overlap counts once (nonzero)");
}

void clockwise_outlines_fill_the_same() {
    GlyphOutline o = outline_of(600);
    add_rect(o, 100, 0, 500, 400, false);  // TrueType outer contours run clockwise
    PT_ASSERT(near(fill_area(fill_only(o)), 0.16, 1e-6), "orientation does not matter for nonzero");
}

void crossing_contours_split_into_slabs() {
    // A bow tie: the two lobes meet at a crossing, which must become a slab boundary.
    GlyphOutline o = outline_of(600);
    auto pt = [](SvgPathCommand c, float x, float y) {
        SvgPathPoint p;
        p.command = c;
        p.x = x;
        p.y = y;
        return p;
    };
    o.path.push_back(pt(SvgPathCommand::MOVE_TO, 0, 0));
    o.path.push_back(pt(SvgPathCommand::LINE_TO, 400, 400));
    o.path.push_back(pt(SvgPathCommand::LINE_TO, 400, 0));
    o.path.push_back(pt(SvgPathCommand::LINE_TO, 0, 400));
    o.path.push_back(pt(SvgPathCommand::CLOSE, 0, 0));
    PT_ASSERT(near(fill_area(fill_only(o)), 0.08, 1e-5), "two triangles of 0.04 em^2 each");
}

void curves_follow_the_circle() {
    GlyphOutline o = outline_of(1000);
    add_circle(o, 500, 500, 400);
    const double expected = 3.14159265358979 * 0.4 * 0.4;
    const double area = fill_area(fill_only(o));
    PT_ASSERT(near(area, expected, expected * 0.01), "flattened circle within 1%");
    // A coarser tier uses fewer points.
    const GlyphContours fine = flatten_glyph_contours(o, glyph_tier_tolerance_em(8));
    const GlyphContours coarse = flatten_glyph_contours(o, glyph_tier_tolerance_em(1));
    PT_ASSERT(fine.points.size() > coarse.points.size(), "larger tiers are flattened more finely");
}

void feather_only_on_the_outer_boundary() {
    GlyphOutline single = outline_of(600);
    add_rect(single, 0, 0, 400, 400, true);
    const GlyphMeshData one = build_glyph_mesh(single, kEm, 4);
    // 4 edges x 2 outer vertices + 4 corners x 2 wedge vertices.
    PT_ASSERT_OP(feather_vertex_count(one), ==, 16, "square band");
    for (const GlyphVertex& v : one.vertices) {
        if (v.offset_x == 0.0f && v.offset_y == 0.0f) continue;
        // Outward: the offset points away from the square centre.
        const float dot = (v.x - 0.2f) * v.offset_x + (v.y - 0.2f) * v.offset_y;
        PT_ASSERT(dot > 0.0f, "feather points away from the fill");
        PT_ASSERT(v.coverage == 0.0f, "outer feather vertices fade to zero");
    }

    // Two overlapping squares: the edges buried in the overlap get no band.
    GlyphOutline overlap = outline_of(600);
    add_rect(overlap, 0, 0, 400, 400, true);
    add_rect(overlap, 200, 200, 600, 600, true);
    const GlyphMeshData two = build_glyph_mesh(overlap, kEm, 4);
    size_t buried = 0;
    for (const GlyphVertex& v : two.vertices) {
        const bool inside_both = v.x > 0.2f + 1e-4f && v.x < 0.4f - 1e-4f && v.y > 0.2f + 1e-4f && v.y < 0.4f - 1e-4f;
        if ((v.offset_x != 0.0f || v.offset_y != 0.0f) && inside_both) ++buried;
    }
    PT_ASSERT_OP(buried, ==, 0, "no feather inside the overlap");
    PT_ASSERT(feather_vertex_count(two) > 0, "outer edges of the union still feather");
}

void malformed_outlines_throw() {
    GlyphOutline o = outline_of(600);
    add_rect(o, 0, 0, NAN, 400, true);
    bool threw = false;
    try {
        (void)build_glyph_mesh(o, kEm, 3);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    PT_ASSERT(threw, "non-finite points are rejected");
}

void size_tiers_cover_doubling_sizes() {
    PT_ASSERT_OP(glyph_size_tier(8.0f), ==, 0, "8 px is tier 0");
    PT_ASSERT_OP(glyph_size_tier(9.0f), ==, 1, "9 px is tier 1");
    PT_ASSERT_OP(glyph_size_tier(16.0f), ==, 1, "16 px is tier 1");
    PT_ASSERT_OP(glyph_size_tier(100000.0f), ==, kGlyphSizeTierCount - 1, "huge sizes use the last tier");
    PT_ASSERT(glyph_tier_tolerance_em(0) > glyph_tier_tolerance_em(5), "bigger tiers flatten finer");
}

/// A fake font: 'A' and 'B' are squares, ' ' is blank, kerning A-B is -100 units.
class FakeSource final : public IGlyphOutlineSource {
public:
    mutable int built = 0;
    GlyphOutline outline(FontHandle, uint32_t codepoint) const override {
        ++built;
        GlyphOutline o = outline_of(codepoint == ' ' ? 250.0f : 500.0f);
        if (codepoint != ' ') add_rect(o, 50, 0, 450, 700, false);
        return o;
    }
    float units_per_em(FontHandle) const override { return kEm; }
    float kerning(FontHandle, uint32_t l, uint32_t r) const override { return (l == 'A' && r == 'B') ? -100.0f : 0.0f; }
    float line_height(FontHandle) const override { return 1200.0f; }
};

void cache_builds_each_glyph_once_per_tier() {
    FakeSource source;
    GlyphMeshCache cache(source);
    const uint32_t a = cache.mesh_for(0, 'A', 2);
    const uint32_t again = cache.mesh_for(0, 'A', 2);
    const uint32_t bigger = cache.mesh_for(0, 'A', 5);
    const uint32_t other_font = cache.mesh_for(1, 'A', 2);
    PT_ASSERT_OP(a, ==, again, "same key, same mesh");
    PT_ASSERT(bigger != a && other_font != a, "tier and font are part of the key");
    PT_ASSERT_OP(source.built, ==, 3, "built three times");
    const GlyphMeshRange& r = cache.range(a);
    PT_ASSERT(r.index_count > 0 && r.first_index + r.index_count <= cache.indices().size(), "range in bounds");
    for (uint32_t i = 0; i < r.index_count; ++i) {
        PT_ASSERT(r.first_vertex + cache.indices()[r.first_index + i] < cache.vertices().size(), "index in bounds");
    }
    // Many glyphs force the table to grow; earlier ids stay valid.
    for (uint32_t cp = 0x4E00; cp < 0x4E00 + 200; ++cp) (void)cache.mesh_for(0, cp, 2);
    PT_ASSERT_OP(cache.mesh_for(0, 'A', 2), ==, a, "lookup survives rehash");
}

void layout_places_glyphs_on_the_baseline() {
    FakeSource source;
    GlyphMeshCache cache(source);
    VectorTextBatch batch;
    VectorTextStyle style;
    style.pixels_per_em = 20.0f;
    const float width = layout_vector_text(cache, 0, "AB A\nB", style, 10.0f, 100.0f, batch);
    // Line 1: A (10) + kern (-2) + B (10) + space (5) + A (10) = 33 px; line 2: B = 10 px.
    PT_ASSERT(near(width, 33.0, 1e-4), "longest line width with kerning");
    PT_ASSERT_OP(batch.instances().size(), ==, 4, "blank glyphs are not drawn");
    const VectorTextInstance& b = batch.instances()[1];
    PT_ASSERT(near(b.tx, 10.0 + 10.0 - 2.0, 1e-4) && near(b.ty, 100.0, 1e-4), "B follows A with kerning");
    PT_ASSERT(near(b.m[0], 20.0, 1e-4) && near(b.m[3], -20.0, 1e-4), "em y-up maps to screen y-down");
    const VectorTextInstance& second_line = batch.instances()[3];
    PT_ASSERT(near(second_line.tx, 10.0, 1e-4) && near(second_line.ty, 124.0, 1e-4), "new line steps by line height");
    PT_ASSERT(near(measure_vector_text(cache, 0, "AB A\nB", 20.0f), width, 1e-4), "measure matches layout");

    batch.build_draws();
    size_t drawn = 0;
    for (const VectorTextDraw& d : batch.draws()) {
        drawn += d.instance_count;
        for (uint32_t i = 0; i < d.instance_count; ++i) {
            PT_ASSERT_OP(batch.meshes()[d.first_instance + i], ==, d.mesh, "draw groups one mesh");
        }
    }
    PT_ASSERT_OP(drawn, ==, 4, "every instance is in one draw");
    PT_ASSERT_OP(batch.draws().size(), ==, 2, "two distinct glyphs, two draws");
}

void rotated_layout_runs_along_the_rotated_baseline() {
    FakeSource source;
    GlyphMeshCache cache(source);
    VectorTextBatch batch;
    VectorTextStyle style;
    style.pixels_per_em = 20.0f;
    style.rotation_rad  = 3.14159265f / 2.0f;  // quarter turn clockwise: text runs down the screen
    (void)layout_vector_text(cache, 0, "AA", style, 0.0f, 0.0f, batch);
    const VectorTextInstance& second = batch.instances()[1];
    PT_ASSERT(near(second.tx, 0.0, 1e-3) && near(second.ty, 10.0, 1e-3), "pen moves down the screen");
}

void utf8_decoding() {
    size_t at = 0;
    const std::string text = "a\xE3\x81\x82\xF0\x9F\x98\x80\xFF";
    PT_ASSERT_OP(next_utf8_codepoint(text, at), ==, 'a', "ascii");
    PT_ASSERT_OP(next_utf8_codepoint(text, at), ==, 0x3042, "hiragana a");
    PT_ASSERT_OP(next_utf8_codepoint(text, at), ==, 0x1F600, "four-byte");
    PT_ASSERT_OP(next_utf8_codepoint(text, at), ==, 0xFFFD, "invalid byte");
    PT_ASSERT_OP(at, ==, text.size(), "consumed");
    at = 0;
    const std::string overlong = "\xC0\xAF";
    PT_ASSERT_OP(next_utf8_codepoint(overlong, at), ==, 0xFFFD, "overlong form rejected");
}

}  // namespace

int main() {
    square_fills_its_area();
    hole_with_opposite_winding_is_cut_out();
    same_winding_overlap_is_a_union();
    clockwise_outlines_fill_the_same();
    crossing_contours_split_into_slabs();
    curves_follow_the_circle();
    feather_only_on_the_outer_boundary();
    malformed_outlines_throw();
    size_tiers_cover_doubling_sizes();
    cache_builds_each_glyph_once_per_tier();
    layout_places_glyphs_on_the_baseline();
    rotated_layout_runs_along_the_rotated_baseline();
    utf8_decoding();
    return report("unit_vector_text_test");
}
