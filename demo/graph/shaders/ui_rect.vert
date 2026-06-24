#version 450

// Instanced screen-space solid rect (dock chrome). Geometry from gl_VertexIndex.

layout(push_constant) uniform PushConstants {
    mat4 proj;      // screen px -> clip
} pc;

struct UiRect {
    vec4 rect;      // x, y, w, h  (pixels)
    vec4 color;     // rgba 0..1
};

layout(std430, set = 0, binding = 0) readonly buffer Rects {
    UiRect rects[];
};

layout(location = 0) out vec4 vColor;

vec2 corner(int i) {
    // Unit quad in [0,1], two triangles.
    if (i == 0) return vec2(0.0, 0.0);
    if (i == 1) return vec2(1.0, 0.0);
    if (i == 2) return vec2(1.0, 1.0);
    if (i == 3) return vec2(0.0, 0.0);
    if (i == 4) return vec2(1.0, 1.0);
    return vec2(0.0, 1.0);
}

void main() {
    UiRect r = rects[gl_InstanceIndex];
    vec2 q = corner(gl_VertexIndex);
    vec2 px = r.rect.xy + q * r.rect.zw;
    gl_Position = pc.proj * vec4(px, 0.0, 1.0);
    vColor = r.color;
}
