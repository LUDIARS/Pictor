#version 450

// Instanced node quad. Geometry comes purely from gl_VertexIndex (6 verts,
// two triangles) + a flat SSBO record per instance — no vertex buffer.

layout(push_constant) uniform PushConstants {
    mat4 proj;      // screen px -> clip
    mat4 view;      // world -> screen px (pan/zoom)
    vec4 params;    // x = zoom (unused for nodes)
} pc;

struct NodeInstance {
    vec4 rect;      // cx, cy, w, h  (world units)
    vec4 color;     // rgba 0..1
};

layout(std430, set = 0, binding = 0) readonly buffer Nodes {
    NodeInstance nodes[];
};

layout(location = 0) out vec4 vColor;

vec2 corner(int i) {
    // CCW unit quad in [-0.5, 0.5], two triangles.
    if (i == 0) return vec2(-0.5, -0.5);
    if (i == 1) return vec2( 0.5, -0.5);
    if (i == 2) return vec2( 0.5,  0.5);
    if (i == 3) return vec2(-0.5, -0.5);
    if (i == 4) return vec2( 0.5,  0.5);
    return vec2(-0.5, 0.5);
}

void main() {
    NodeInstance n = nodes[gl_InstanceIndex];
    vec2 local = corner(gl_VertexIndex);
    vec2 world = n.rect.xy + local * n.rect.zw;
    gl_Position = pc.proj * pc.view * vec4(world, 0.0, 1.0);
    vColor = n.color;
}
