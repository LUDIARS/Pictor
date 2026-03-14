#include "pictor/profiler/stats_overlay.h"
#include <cstdio>
#include <cstring>

namespace pictor {

void StatsOverlay::initialize(uint32_t screen_width, uint32_t screen_height) {
    screen_width_ = screen_width;
    screen_height_ = screen_height;
    initialized_ = true;
}

#ifdef PICTOR_HAS_VULKAN
void StatsOverlay::begin_render(VkCommandBuffer cmd, VkExtent2D extent) {
    if (text_renderer_) {
        text_renderer_->begin(cmd, extent);
    }
}

void StatsOverlay::end_render() {
    if (text_renderer_) {
        text_renderer_->end();
    }
}
#endif

void StatsOverlay::render(const FrameStats& stats, const SceneSummary& summary) {
    if (!initialized_ || !visible_) return;

    float x = MARGIN_X;
    float y = MARGIN_Y;

    char buf[256];

    std::snprintf(buf, sizeof(buf), "FPS: %.1f  (%.2f ms)", stats.fps, stats.frame_time_ms);
    draw_text(x, y, buf);
    y += LINE_HEIGHT;

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

    std::snprintf(buf, sizeof(buf),
                  "Batches: %u  Polygons: %llu  DrawCalls: %u",
                  summary.batch_count,
                  static_cast<unsigned long long>(summary.polygon_count),
                  summary.draw_call_count);
    draw_text(x, y, buf);
    y += LINE_HEIGHT;

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
#ifdef PICTOR_HAS_VULKAN
    if (text_renderer_) {
        text_renderer_->draw_text(x, y, text, 0.0f, 1.0f, 0.0f, 1.0f);
        return;
    }
#endif
#ifdef PICTOR_STATS_DEBUG
    printf("[Stats %4.0f,%4.0f] %s\n", x, y, text);
#endif
    (void)x;
    (void)y;
    (void)text;
}

} // namespace pictor
