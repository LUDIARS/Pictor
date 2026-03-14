#include "pictor/profiler/stats_overlay.h"
#include <cstdio>
#include <cstring>

namespace pictor {

void StatsOverlay::initialize(uint32_t screen_width, uint32_t screen_height) {
    screen_width_ = screen_width;
    screen_height_ = screen_height;

    // In a full implementation:
    // - Reuse SDF font atlas from OverlayRenderer
    // - Create dedicated quad batch for stats text
    // - Allocate small vertex buffer for overlay geometry

    initialized_ = true;
}

void StatsOverlay::render(const FrameStats& stats, const SceneSummary& summary) {
    if (!initialized_ || !visible_) return;

    // Render stats text at top-left corner.
    // In production this uses the SDF text rendering pipeline.
    // Current implementation records draw commands for text quads
    // that will be submitted after the post-process pass.

    float x = MARGIN_X;
    float y = MARGIN_Y;

    // ---- Frame rate ----
    char buf[256];

    std::snprintf(buf, sizeof(buf), "FPS: %.1f  (%.2f ms)", stats.fps, stats.frame_time_ms);
    draw_text(x, y, buf);
    y += LINE_HEIGHT;

    // ---- CPU / GPU load ----
    // CPU load: sum of per-pass CPU times relative to frame budget (16.67ms@60fps)
    double cpu_total_ms = stats.data_update_ms + stats.culling_ms +
                          stats.sort_ms + stats.batch_build_ms +
                          stats.command_encode_ms;
    double cpu_load_pct = (stats.frame_time_ms > 0.0)
                          ? (cpu_total_ms / stats.frame_time_ms) * 100.0
                          : 0.0;

    double gpu_total_ms = stats.shadow_gpu_ms + stats.depth_prepass_gpu_ms +
                          stats.opaque_gpu_ms + stats.transparent_gpu_ms +
                          stats.post_process_gpu_ms + stats.compute_update_gpu_ms +
                          stats.gpu_cull_ms;
    double gpu_load_pct = (stats.frame_time_ms > 0.0)
                          ? (gpu_total_ms / stats.frame_time_ms) * 100.0
                          : 0.0;

    std::snprintf(buf, sizeof(buf),
                  "CPU: %.1f ms (%.0f%%)  GPU: %.1f ms (%.0f%%)",
                  cpu_total_ms, cpu_load_pct,
                  gpu_total_ms, gpu_load_pct);
    draw_text(x, y, buf);
    y += LINE_HEIGHT;

    // ---- Draw statistics ----
    std::snprintf(buf, sizeof(buf),
                  "Batches: %u  Polygons: %llu  DrawCalls: %u",
                  summary.batch_count,
                  static_cast<unsigned long long>(summary.polygon_count),
                  summary.draw_call_count);
    draw_text(x, y, buf);
    y += LINE_HEIGHT;

    // ---- Lighting / GI / Shadow ----
    const char* filter_str = "OFF";
    if (summary.shadow_enabled) {
        switch (summary.shadow_filter_mode) {
            case ShadowFilterMode::NONE: filter_str = "Hard"; break;
            case ShadowFilterMode::PCF:  filter_str = "PCF";  break;
            case ShadowFilterMode::PCSS: filter_str = "PCSS"; break;
        }
    }

    std::snprintf(buf, sizeof(buf),
                  "Light: %s  GI: %s  Shadow: %s",
                  summary.light_enabled  ? "ON" : "OFF",
                  summary.gi_enabled     ? "ON" : "OFF",
                  summary.shadow_enabled ? "ON" : "OFF");
    draw_text(x, y, buf);
    y += LINE_HEIGHT;

    if (summary.shadow_enabled) {
        std::snprintf(buf, sizeof(buf),
                      "  Filter: %s  Cascades: %u  Resolution: %u",
                      filter_str,
                      summary.shadow_cascades,
                      summary.shadow_resolution);
        draw_text(x, y, buf);
        y += LINE_HEIGHT;
    }
}

void StatsOverlay::resize(uint32_t width, uint32_t height) {
    screen_width_ = width;
    screen_height_ = height;
}

void StatsOverlay::draw_text(float x, float y, const char* text) {
    // In production: generate textured quads from SDF font atlas,
    // append to overlay vertex buffer for batched rendering.
    //
    // Stub: record the text position and content.
    // When PICTOR_STATS_DEBUG is defined, also print to stdout.
#ifdef PICTOR_STATS_DEBUG
    printf("[Stats %4.0f,%4.0f] %s\n", x, y, text);
#endif
    (void)x;
    (void)y;
    (void)text;
}

} // namespace pictor
