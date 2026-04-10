#pragma once

#include "pictor/core/types.h"
#include <cstdint>
#include <cstring>

namespace pictor {

// ─── UI Vertex (per-vertex data for batched rendering) ───────

struct UIVertex {
    float pos[2];      // screen-space position
    float uv[2];       // texture coordinates
    float color[4];    // RGBA tint (premultiplied alpha)
};

// ─── Overlay Element ─────────────────────────────────────────
// A single UI quad. Can be static (batched) or dynamic (updated per frame).

using OverlayElementId = uint32_t;
constexpr OverlayElementId INVALID_OVERLAY_ELEMENT = UINT32_MAX;

struct OverlayElement {
    OverlayElementId id = INVALID_OVERLAY_ELEMENT;

    // Transform (screen-space pixels)
    float x      = 0.0f;
    float y      = 0.0f;
    float width  = 100.0f;
    float height = 100.0f;
    float rotation = 0.0f;   // degrees

    // Appearance
    float color[4] = {1.0f, 1.0f, 1.0f, 1.0f};  // RGBA
    TextureHandle texture = INVALID_TEXTURE;

    // UV region (for atlas support)
    float uv_min[2] = {0.0f, 0.0f};
    float uv_max[2] = {1.0f, 1.0f};

    // Flags
    bool  is_static = true;    // static = batched, dynamic = updated per frame
    bool  visible   = true;

    // Z-order within group (lower = drawn first = behind)
    int32_t z_order = 0;
};

// ─── Overlay Group ID ────────────────────────────────────────

using OverlayGroupId = uint32_t;
constexpr OverlayGroupId INVALID_OVERLAY_GROUP = UINT32_MAX;

// ─── Batch Stats ─────────────────────────────────────────────

struct OverlayBatchStats {
    uint32_t total_groups       = 0;
    uint32_t total_elements     = 0;
    uint32_t static_elements    = 0;
    uint32_t dynamic_elements   = 0;
    uint32_t draw_calls         = 0;
    uint32_t vertices_uploaded  = 0;
    uint32_t batch_rebuilds     = 0;  // since last reset
};

} // namespace pictor
