#pragma once

/// pictor::text_effects — post-process effects for rasterized text.
///
/// Operates on the **alpha bitmap** produced by `TextImageRenderer`
/// (or any 1-channel glyph coverage map). Each effect returns a new
/// RGBA8 `ImageBuffer` sized to fit the text plus the effect padding
/// (e.g., shadow offsets, glow radius); the caller is expected to
/// composite the returned layer with the original fill.
///
/// These live in `src/text/` (not `src/postprocess/`) because they are
/// text-specific — small-region CPU operations, not full-frame GPU
/// passes. For full-frame Vulkan blurs see `GaussianBlurEffect`.
///
/// Typical usage:
///
/// ```cpp
/// auto fill      = TextImageRenderer::render_text(...);       // RGBA8
/// auto alpha     = extract_alpha(fill);                        // 1-ch
/// auto outline   = text_effects::apply_outline (alpha, {.width_px = 2});
/// auto shadow    = text_effects::apply_drop_shadow(alpha, {.offset_y = 2});
/// auto glow      = text_effects::apply_glow     (alpha, {.radius = 6, .r=255});
/// // composite order: glow → shadow → outline → fill
/// ```

#include <cstdint>

#include "pictor/text/text_types.h"

namespace pictor::text_effects {

// ─── RGBA8 colour (premultiplied alpha *not* assumed) ─────────
struct RgbaColor {
    uint8_t r = 0, g = 0, b = 0, a = 255;
};

// ─── Outline ──────────────────────────────────────────────────
struct OutlineParams {
    /// Outline thickness in pixels. 0 disables the effect.
    uint32_t   width_px    = 1;
    /// Outline colour drawn behind the glyph fill.
    RgbaColor  color       = { 0, 0, 0, 255 };
    /// Soft edge (0=crisp, 1=maximally blurred). Implemented as a
    /// couple of box-blur passes over the dilated alpha.
    float      softness    = 0.0f;
};

/// Dilate the glyph alpha by `width_px` and tint with `color`. The
/// returned RGBA buffer is sized to `src.width + 2*width_px` by
/// `src.height + 2*width_px` so the outline fits around the text.
ImageBuffer apply_outline(const ImageBuffer& alpha_src, const OutlineParams& params);

// ─── Drop shadow ─────────────────────────────────────────────
struct DropShadowParams {
    int32_t    offset_x_px = 2;
    int32_t    offset_y_px = 2;
    /// Blur radius in pixels. 0 gives a hard shadow (cheapest).
    uint32_t   blur_radius = 2;
    RgbaColor  color       = { 0, 0, 0, 180 };
};

/// Produce a standalone shadow layer. Caller composites it under the
/// glyph fill at the same origin (the returned buffer is already
/// offset-padded so the shadow peeks out from behind).
ImageBuffer apply_drop_shadow(const ImageBuffer& alpha_src, const DropShadowParams& params);

// ─── Glow ────────────────────────────────────────────────────
struct GlowParams {
    /// Blur radius in pixels — the glow's soft halo width.
    uint32_t   radius_px   = 6;
    /// Multiplier applied to the blurred alpha before colouring.
    /// Values > 1 brighten the glow core; 1.0 gives a soft halo.
    float      intensity   = 1.5f;
    RgbaColor  color       = { 200, 220, 255, 255 };
    /// Additive blend is the common "neon" look.
    bool       additive    = true;
};

/// Produce a glow layer centred on the glyph shape. Caller composites
/// it **under** the fill (or additively blends it with what's already
/// in the framebuffer).
ImageBuffer apply_glow(const ImageBuffer& alpha_src, const GlowParams& params);

// ─── Compositing helpers ─────────────────────────────────────

/// `dst_over_src` — place `src` (a new effect layer) behind `dst`,
/// which is the fill layer. Both must be RGBA8 and same dimensions.
void composite_under(ImageBuffer& dst, const ImageBuffer& src);

/// `dst_over_src_additive` — add `src` to `dst`. RGBA8 same dims.
void composite_additive(ImageBuffer& dst, const ImageBuffer& src);

/// Extract the alpha channel from an RGBA8 buffer into a 1-ch buffer.
/// Handy shim when the caller already has a fill bitmap.
ImageBuffer extract_alpha(const ImageBuffer& rgba_src);

} // namespace pictor::text_effects
