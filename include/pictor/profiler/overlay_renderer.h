#pragma once

#include "pictor/core/types.h"
#include "pictor/profiler/profiler.h"

namespace pictor {

/// Profiler overlay UI renderer (§13.6).
/// Renders stats as debug overlay using SDF text.
/// Executed after PostProcess pass to avoid affecting render output.
class OverlayRenderer {
public:
    OverlayRenderer() = default;
    ~OverlayRenderer() = default;

    /// Initialize overlay resources (SDF font atlas, quad shader)
    void initialize(uint32_t screen_width, uint32_t screen_height);

    /// Render overlay based on current mode and stats
    void render(OverlayMode mode, const FrameStats& stats, const Profiler& profiler);

    /// Update screen dimensions
    void resize(uint32_t width, uint32_t height);

private:
    /// Render MINIMAL mode: FPS + Frame Time (§13.6)
    void render_minimal(const FrameStats& stats);

    /// Render STANDARD mode: + graphs + pass bars + draw calls (§13.6)
    void render_standard(const FrameStats& stats, const Profiler& profiler);

    /// Render DETAILED mode: + memory + draw stats + timeline (§13.6)
    void render_detailed(const FrameStats& stats, const Profiler& profiler);

    /// Render TIMELINE mode: GPU/CPU timeline bars (§13.6)
    void render_timeline(const Profiler& profiler);

    uint32_t screen_width_  = 1920;
    uint32_t screen_height_ = 1080;
    bool     initialized_   = false;
};

} // namespace pictor
