#include "pictor/profiler/overlay_renderer.h"
#include <cstdio>

namespace pictor {

void OverlayRenderer::initialize(uint32_t screen_width, uint32_t screen_height) {
    screen_width_ = screen_width;
    screen_height_ = screen_height;

    // In a real implementation:
    // - Load SDF font atlas texture
    // - Create text rendering shader (quad + SDF sampling)
    // - Create bar/graph rendering shader
    // - Allocate vertex buffer for overlay geometry

    initialized_ = true;
}

void OverlayRenderer::render(OverlayMode mode, const FrameStats& stats,
                              const Profiler& profiler) {
    if (!initialized_ || mode == OverlayMode::OFF) return;

    switch (mode) {
        case OverlayMode::MINIMAL:
            render_minimal(stats);
            break;
        case OverlayMode::STANDARD:
            render_standard(stats, profiler);
            break;
        case OverlayMode::DETAILED:
            render_detailed(stats, profiler);
            break;
        case OverlayMode::TIMELINE:
            render_timeline(profiler);
            break;
        case OverlayMode::OFF:
            break;
    }
}

void OverlayRenderer::resize(uint32_t width, uint32_t height) {
    screen_width_ = width;
    screen_height_ = height;
}

void OverlayRenderer::render_minimal(const FrameStats& stats) {
    // §13.6: MINIMAL — FPS + Frame Time (top-left)
    // In a real implementation, this would render SDF text quads:
    // "FPS: 60 | Frame: 16.7ms"
    (void)stats;
}

void OverlayRenderer::render_standard(const FrameStats& stats, const Profiler& profiler) {
    // §13.6: STANDARD — FPS + graph + pass bars + draw calls
    render_minimal(stats);

    // Frame time line graph (300 frames history)
    // Pass timing horizontal bar chart
    // Draw call count

    (void)profiler;
}

void OverlayRenderer::render_detailed(const FrameStats& stats, const Profiler& profiler) {
    // §13.6: DETAILED — STANDARD + memory + draw stats + timeline
    render_standard(stats, profiler);

    // Memory statistics bars
    // Draw statistics table
    // GPU/CPU timeline
}

void OverlayRenderer::render_timeline(const Profiler& profiler) {
    // §13.6: TIMELINE — GPUView-style horizontal bar graph
    // Shows each compute/render pass as a colored bar
    // Width proportional to execution time

    (void)profiler;
}

} // namespace pictor
