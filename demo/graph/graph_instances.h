#pragma once

namespace pictor::graph {

/// GPU-packed instance records (std430: vec4 + vec4 = 32 bytes), shared between
/// the culler (GraphView, which builds the visible subset each frame) and the
/// renderer (which uploads + draws it).
struct NodeInstance {
    float rect[4];   // cx, cy, w, h  (world units)
    float color[4];  // rgba 0..1
};

struct EdgeInstance {
    float ends[4];   // ax, ay, bx, by  (world units)
    float color[4];  // rgb + thickness(px) in .a
};

/// Screen-space solid rect for the dock chrome (panels / splitters / tab headers).
struct UiRect {
    float rect[4];   // x, y, w, h  (pixels)
    float color[4];  // rgba 0..1
};

} // namespace pictor::graph
