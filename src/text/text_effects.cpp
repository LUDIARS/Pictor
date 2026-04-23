#include "pictor/text/text_effects.h"

#include <algorithm>
#include <cstring>
#include <vector>

namespace pictor::text_effects {

namespace {

// ─── Small helpers over 1-channel alpha buffers ───────────────

bool is_alpha(const ImageBuffer& b) {
    return b.channels == 1 && b.width > 0 && b.height > 0
        && b.pixels.size() == b.byte_size();
}

bool is_rgba(const ImageBuffer& b) {
    return b.channels == 4 && b.width > 0 && b.height > 0
        && b.pixels.size() == b.byte_size();
}

/// Copy the alpha into a padded buffer with `pad` extra cells on every
/// side. Returns the new buffer (width+2*pad x height+2*pad).
ImageBuffer pad_alpha(const ImageBuffer& src, uint32_t pad) {
    ImageBuffer out;
    out.allocate(src.width + 2 * pad, src.height + 2 * pad, 1);
    for (uint32_t y = 0; y < src.height; ++y) {
        std::memcpy(out.pixels.data() + ((y + pad) * out.width + pad),
                    src.pixels.data() + y * src.width,
                    src.width);
    }
    return out;
}

/// Maximum-filter (morphological dilate) for a 1-ch alpha buffer. Each
/// output pixel takes the max of its (2r+1)x(2r+1) neighborhood. A
/// separable implementation (horizontal then vertical) is O(w*h*r)
/// which is fine for small radii (<=8 px) typical for outlines.
void dilate_alpha(ImageBuffer& buf, uint32_t radius) {
    if (radius == 0 || !is_alpha(buf)) return;
    const uint32_t W = buf.width, H = buf.height;
    std::vector<uint8_t> tmp(buf.pixels.size(), 0);

    // Horizontal pass.
    for (uint32_t y = 0; y < H; ++y) {
        const uint8_t* row = buf.pixels.data() + y * W;
        uint8_t* out_row = tmp.data() + y * W;
        for (uint32_t x = 0; x < W; ++x) {
            const uint32_t xs = x >= radius ? x - radius : 0;
            const uint32_t xe = std::min(W, x + radius + 1);
            uint8_t m = 0;
            for (uint32_t xi = xs; xi < xe; ++xi) m = std::max(m, row[xi]);
            out_row[x] = m;
        }
    }

    // Vertical pass.
    for (uint32_t x = 0; x < W; ++x) {
        for (uint32_t y = 0; y < H; ++y) {
            const uint32_t ys = y >= radius ? y - radius : 0;
            const uint32_t ye = std::min(H, y + radius + 1);
            uint8_t m = 0;
            for (uint32_t yi = ys; yi < ye; ++yi) {
                m = std::max(m, tmp[yi * W + x]);
            }
            buf.pixels[y * W + x] = m;
        }
    }
}

/// Box blur on a 1-ch alpha buffer. Uses a moving-sum per row/column
/// which is O(w*h) regardless of radius — ideal for large glow blurs.
/// Runs `passes` iterations (3 passes of a box filter approximates a
/// Gaussian well).
void box_blur_alpha(ImageBuffer& buf, uint32_t radius, uint32_t passes = 2) {
    if (radius == 0 || passes == 0 || !is_alpha(buf)) return;
    const uint32_t W = buf.width, H = buf.height;
    std::vector<uint8_t> tmp(buf.pixels.size(), 0);

    for (uint32_t pass = 0; pass < passes; ++pass) {
        // Horizontal moving average.
        for (uint32_t y = 0; y < H; ++y) {
            const uint8_t* in_row = buf.pixels.data() + y * W;
            uint8_t* out_row = tmp.data() + y * W;
            uint32_t sum = 0;
            const uint32_t kw = radius * 2 + 1;
            // initial window
            for (uint32_t x = 0; x < kw && x < W; ++x) sum += in_row[x];
            for (uint32_t x = 0; x < W; ++x) {
                const uint32_t div = std::min<uint32_t>(kw, std::min<uint32_t>(x + radius + 1, W) - (x >= radius ? x - radius : 0));
                out_row[x] = static_cast<uint8_t>(sum / std::max(1u, div));
                // slide window
                const uint32_t add_x = x + radius + 1;
                const uint32_t sub_x = x >= radius ? x - radius : UINT32_MAX;
                if (add_x < W) sum += in_row[add_x];
                if (sub_x != UINT32_MAX) sum -= in_row[sub_x];
            }
        }
        // Vertical moving average.
        for (uint32_t x = 0; x < W; ++x) {
            uint32_t sum = 0;
            const uint32_t kh = radius * 2 + 1;
            for (uint32_t y = 0; y < kh && y < H; ++y) sum += tmp[y * W + x];
            for (uint32_t y = 0; y < H; ++y) {
                const uint32_t div = std::min<uint32_t>(kh, std::min<uint32_t>(y + radius + 1, H) - (y >= radius ? y - radius : 0));
                buf.pixels[y * W + x] = static_cast<uint8_t>(sum / std::max(1u, div));
                const uint32_t add_y = y + radius + 1;
                const uint32_t sub_y = y >= radius ? y - radius : UINT32_MAX;
                if (add_y < H) sum += tmp[add_y * W + x];
                if (sub_y != UINT32_MAX) sum -= tmp[sub_y * W + x];
            }
        }
    }
}

/// Paint an alpha buffer into an RGBA8 buffer using `col`, multiplying
/// col.a by the alpha value (so fully-opaque alpha renders at
/// `col.a`). The destination must be the same dims as the alpha buf.
ImageBuffer colorize_alpha(const ImageBuffer& alpha, RgbaColor col) {
    ImageBuffer out;
    out.allocate(alpha.width, alpha.height, 4);
    const uint32_t N = alpha.width * alpha.height;
    for (uint32_t i = 0; i < N; ++i) {
        const uint8_t a8 = alpha.pixels[i];
        if (a8 == 0) continue;
        const uint32_t a = (static_cast<uint32_t>(col.a) * a8 + 127) / 255;
        out.pixels[i * 4 + 0] = col.r;
        out.pixels[i * 4 + 1] = col.g;
        out.pixels[i * 4 + 2] = col.b;
        out.pixels[i * 4 + 3] = static_cast<uint8_t>(a);
    }
    return out;
}

} // namespace

// ─── apply_outline ───────────────────────────────────────────

ImageBuffer apply_outline(const ImageBuffer& alpha_src, const OutlineParams& p) {
    if (!is_alpha(alpha_src) || p.width_px == 0) return {};

    // Pad, dilate, optional soft-blur. Padding size = width + softness blur.
    const uint32_t pad = p.width_px + (p.softness > 0.0f ? 2u : 0u);
    ImageBuffer dilated = pad_alpha(alpha_src, pad);
    dilate_alpha(dilated, p.width_px);
    if (p.softness > 0.0f) {
        const uint32_t r = static_cast<uint32_t>(std::max(1.0f, p.softness * static_cast<float>(p.width_px)));
        box_blur_alpha(dilated, r, 1);
    }
    return colorize_alpha(dilated, p.color);
}

// ─── apply_drop_shadow ───────────────────────────────────────

ImageBuffer apply_drop_shadow(const ImageBuffer& alpha_src, const DropShadowParams& p) {
    if (!is_alpha(alpha_src)) return {};

    // Pad enough so the offset + blur doesn't clip. Use absolute values
    // since the shadow can trail either direction.
    const uint32_t pad_xy = static_cast<uint32_t>(
        std::max(std::abs(p.offset_x_px), std::abs(p.offset_y_px))) + p.blur_radius;
    ImageBuffer shifted;
    shifted.allocate(alpha_src.width + 2 * pad_xy,
                     alpha_src.height + 2 * pad_xy, 1);

    // Copy `alpha_src` into `shifted` at (pad_xy + offset_x, pad_xy + offset_y).
    for (uint32_t y = 0; y < alpha_src.height; ++y) {
        const int32_t dy = static_cast<int32_t>(pad_xy) + p.offset_y_px + static_cast<int32_t>(y);
        if (dy < 0 || dy >= static_cast<int32_t>(shifted.height)) continue;
        for (uint32_t x = 0; x < alpha_src.width; ++x) {
            const int32_t dx = static_cast<int32_t>(pad_xy) + p.offset_x_px + static_cast<int32_t>(x);
            if (dx < 0 || dx >= static_cast<int32_t>(shifted.width)) continue;
            shifted.pixels[dy * shifted.width + dx] =
                alpha_src.pixels[y * alpha_src.width + x];
        }
    }
    if (p.blur_radius > 0) {
        box_blur_alpha(shifted, p.blur_radius, 2);
    }
    return colorize_alpha(shifted, p.color);
}

// ─── apply_glow ──────────────────────────────────────────────

ImageBuffer apply_glow(const ImageBuffer& alpha_src, const GlowParams& p) {
    if (!is_alpha(alpha_src)) return {};

    const uint32_t pad = p.radius_px;
    ImageBuffer padded = pad_alpha(alpha_src, pad);
    if (p.radius_px > 0) {
        box_blur_alpha(padded, p.radius_px, 3); // 3 passes ≈ Gaussian
    }

    // Apply intensity by scaling the blurred alpha values. Clamp at 255.
    if (p.intensity != 1.0f) {
        for (auto& v : padded.pixels) {
            const float scaled = static_cast<float>(v) * p.intensity;
            v = static_cast<uint8_t>(std::min(255.0f, std::max(0.0f, scaled)));
        }
    }
    return colorize_alpha(padded, p.color);
}

// ─── compositing ─────────────────────────────────────────────

void composite_under(ImageBuffer& dst, const ImageBuffer& src) {
    if (!is_rgba(dst) || !is_rgba(src)) return;
    if (dst.width != src.width || dst.height != src.height) return;
    const uint32_t N = dst.width * dst.height;
    for (uint32_t i = 0; i < N; ++i) {
        const uint32_t da = dst.pixels[i * 4 + 3];
        if (da == 255) continue;                // fully opaque — src invisible
        const uint32_t sa = src.pixels[i * 4 + 3];
        if (sa == 0) continue;
        const uint32_t ia = 255 - da;           // share of dst still transparent
        // resulting pixel = dst over (src * ia/255)
        auto mix = [&](uint8_t dc, uint8_t sc) -> uint8_t {
            return static_cast<uint8_t>(((dc * 255) + (sc * ia)) / 255);
        };
        dst.pixels[i * 4 + 0] = mix(dst.pixels[i * 4 + 0], src.pixels[i * 4 + 0]);
        dst.pixels[i * 4 + 1] = mix(dst.pixels[i * 4 + 1], src.pixels[i * 4 + 1]);
        dst.pixels[i * 4 + 2] = mix(dst.pixels[i * 4 + 2], src.pixels[i * 4 + 2]);
        dst.pixels[i * 4 + 3] = static_cast<uint8_t>(std::min<uint32_t>(255, da + (sa * ia) / 255));
    }
}

void composite_additive(ImageBuffer& dst, const ImageBuffer& src) {
    if (!is_rgba(dst) || !is_rgba(src)) return;
    if (dst.width != src.width || dst.height != src.height) return;
    const uint32_t N = dst.width * dst.height;
    for (uint32_t i = 0; i < N; ++i) {
        const uint32_t sa = src.pixels[i * 4 + 3];
        if (sa == 0) continue;
        auto add = [&](uint8_t dc, uint8_t sc) -> uint8_t {
            const uint32_t contrib = (sc * sa) / 255;
            return static_cast<uint8_t>(std::min<uint32_t>(255, dc + contrib));
        };
        dst.pixels[i * 4 + 0] = add(dst.pixels[i * 4 + 0], src.pixels[i * 4 + 0]);
        dst.pixels[i * 4 + 1] = add(dst.pixels[i * 4 + 1], src.pixels[i * 4 + 1]);
        dst.pixels[i * 4 + 2] = add(dst.pixels[i * 4 + 2], src.pixels[i * 4 + 2]);
        dst.pixels[i * 4 + 3] = static_cast<uint8_t>(std::min<uint32_t>(255,
            dst.pixels[i * 4 + 3] + sa));
    }
}

ImageBuffer extract_alpha(const ImageBuffer& rgba) {
    if (!is_rgba(rgba)) return {};
    ImageBuffer out;
    out.allocate(rgba.width, rgba.height, 1);
    const uint32_t N = rgba.width * rgba.height;
    for (uint32_t i = 0; i < N; ++i) out.pixels[i] = rgba.pixels[i * 4 + 3];
    return out;
}

} // namespace pictor::text_effects
