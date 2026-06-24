#pragma once

namespace pictor::graph {

/// Pannable / zoomable 2D camera for the relation-graph view.
///
/// Maps world units → screen pixels: screen = (world - center) * zoom + viewport/2.
/// The view is rebuilt every frame as a plain affine matrix; the camera itself
/// holds only the world point sitting at the viewport center plus the zoom.
class Camera2D {
public:
    /// Drag the view by a screen-space pixel delta (content follows the cursor).
    void pan_pixels(float dx, float dy);

    /// Zoom by `factor` while keeping the world point under (screen_x, screen_y)
    /// fixed on screen. vw/vh are the current viewport size in pixels.
    void zoom_at(float screen_x, float screen_y, float factor, float vw, float vh);

    void reset(float center_x, float center_y, float zoom);

    float zoom()     const { return zoom_; }
    float center_x() const { return cx_; }
    float center_y() const { return cy_; }

    /// world → screen(px) affine, column-major (matches demo mat helpers).
    void build_view(float* out16, float vw, float vh) const;

    /// screen(px) → clip ortho, column-major. Y points down (row 0 = top).
    static void build_proj(float* out16, float vw, float vh);

private:
    float cx_   = 0.0f;   // world point at viewport center
    float cy_   = 0.0f;
    float zoom_ = 1.0f;

    static constexpr float kMinZoom = 0.01f;
    static constexpr float kMaxZoom = 64.0f;
};

} // namespace pictor::graph
