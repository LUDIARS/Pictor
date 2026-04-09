#pragma once

#include "pictor/core/types.h"
#include "pictor/profiler/profiler.h"
#include <cstdio>

#ifdef PICTOR_HAS_VULKAN
#include "pictor/profiler/bitmap_text_renderer.h"
#endif

namespace pictor {

/// Scene summary information for stats overlay display.
/// Aggregates key pipeline state into a single snapshot.
struct SceneSummary {
    // Draw statistics
    uint32_t batch_count      = 0;
    uint64_t polygon_count    = 0;   // triangle_count from FrameStats
    uint32_t draw_call_count  = 0;

    // Lighting / GI state
    bool     light_enabled    = false;
    bool     gi_enabled       = false;

    // Shadow settings
    bool              shadow_enabled     = false;
    ShadowFilterMode  shadow_filter_mode = ShadowFilterMode::NONE;
    uint32_t          shadow_cascades    = 0;
    uint32_t          shadow_resolution  = 0;
};

/// Stats overlay drawn at screen top-left, toggled by S key.
///
/// Displays:
///   - Scene summary (batches, polygons, draw calls, light/GI/shadow)
///   - Frame rate (FPS / frame time)
///   - CPU / GPU load
class StatsOverlay {
public:
    StatsOverlay() = default;
    ~StatsOverlay() = default;

    /// Initialize overlay resources
    void initialize(uint32_t screen_width, uint32_t screen_height);

#ifdef PICTOR_HAS_VULKAN
    /// Set the bitmap text renderer for Vulkan-based text rendering.
    /// Must be called after initialize() and before render().
    void set_text_renderer(BitmapTextRenderer* renderer) { text_renderer_ = renderer; }
#endif

    /// Toggle visibility (bound to S key)
    void toggle() { visible_ = !visible_; }

    bool is_visible() const { return visible_; }
    void set_visible(bool v) { visible_ = v; }

    /// Render stats overlay at top-left corner.
    /// Call after all scene rendering is complete.
    void render(const FrameStats& stats, const SceneSummary& summary);

#ifdef PICTOR_HAS_VULKAN
    /// Begin text rendering within an active render pass.
    void begin_render(VkCommandBuffer cmd, VkExtent2D extent);

    /// End text rendering (flushes batched draw call).
    void end_render();
#endif

    /// Update screen dimensions on resize
    void resize(uint32_t width, uint32_t height);

private:
    void draw_text(float x, float y, const char* text);

    uint32_t screen_width_  = 1920;
    uint32_t screen_height_ = 1080;
    bool     initialized_   = false;
    bool     visible_       = false;

#ifdef PICTOR_HAS_VULKAN
    BitmapTextRenderer* text_renderer_ = nullptr;
#endif

    // Layout constants (pixels, top-left origin)
    static constexpr float MARGIN_X    = 10.0f;
    static constexpr float MARGIN_Y    = 10.0f;
    static constexpr float LINE_HEIGHT = 18.0f;
};

} // namespace pictor
