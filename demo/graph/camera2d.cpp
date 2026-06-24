#include "camera2d.h"

#include <algorithm>
#include <cstring>

namespace pictor::graph {

void Camera2D::pan_pixels(float dx, float dy) {
    // Dragging right should move the content right, i.e. the world point at the
    // center moves left by the screen delta scaled back into world units.
    cx_ -= dx / zoom_;
    cy_ -= dy / zoom_;
}

void Camera2D::zoom_at(float screen_x, float screen_y, float factor,
                       float vw, float vh) {
    const float hw = vw * 0.5f;
    const float hh = vh * 0.5f;

    // World point currently under the cursor.
    const float wx = cx_ + (screen_x - hw) / zoom_;
    const float wy = cy_ + (screen_y - hh) / zoom_;

    zoom_ = std::clamp(zoom_ * factor, kMinZoom, kMaxZoom);

    // Re-anchor the center so that same world point stays under the cursor.
    cx_ = wx - (screen_x - hw) / zoom_;
    cy_ = wy - (screen_y - hh) / zoom_;
}

void Camera2D::reset(float center_x, float center_y, float zoom) {
    cx_   = center_x;
    cy_   = center_y;
    zoom_ = std::clamp(zoom, kMinZoom, kMaxZoom);
}

void Camera2D::build_view(float* m, float vw, float vh) const {
    std::memset(m, 0, 16 * sizeof(float));
    m[0]  = zoom_;
    m[5]  = zoom_;
    m[10] = 1.0f;
    m[15] = 1.0f;
    m[12] = vw * 0.5f - cx_ * zoom_;
    m[13] = vh * 0.5f - cy_ * zoom_;
}

void Camera2D::build_proj(float* m, float vw, float vh) {
    // ortho(l=0, r=vw, b=vh, t=0, n=-1, f=1): screen px → clip, Y down.
    std::memset(m, 0, 16 * sizeof(float));
    m[0]  =  2.0f / vw;
    m[5]  = -2.0f / vh;
    m[10] = -1.0f;          // -2/(f-n) with n=-1,f=1
    m[12] = -1.0f;
    m[13] =  1.0f;
    m[15] =  1.0f;
}

} // namespace pictor::graph
