#version 450

// Instanced edge: a unit quad stretched along the segment a->b, with a
// screen-constant thickness (thickness px / zoom -> world units).

layout(push_constant) uniform PushConstants {
    mat4 proj;      // screen px -> clip
    mat4 view;      // world -> screen px (pan/zoom)
    vec4 params;    // x = zoom
} pc;

struct EdgeInstance {
    vec4 ends;      // ax, ay, bx, by  (world units)
    vec4 color;     // rgb + thickness(px) in .a
};

layout(std430, set = 0, binding = 0) readonly buffer Edges {
    EdgeInstance edges[];
};

layout(location = 0) out vec4 vColor;

vec2 corner(int i) {
    // x in [0,1] = t along the segment, y in [-0.5,0.5] = side.
    if (i == 0) return vec2(0.0, -0.5);
    if (i == 1) return vec2(1.0, -0.5);
    if (i == 2) return vec2(1.0,  0.5);
    if (i == 3) return vec2(0.0, -0.5);
    if (i == 4) return vec2(1.0,  0.5);
    return vec2(0.0, 0.5);
}

void main() {
    EdgeInstance e = edges[gl_InstanceIndex];
    vec2 a = e.ends.xy;
    vec2 b = e.ends.zw;
    vec2 q = corner(gl_VertexIndex);

    vec2 delta = b - a;
    float len = length(delta);
    vec2 dir = (len > 1e-6) ? delta / len : vec2(1.0, 0.0);
    vec2 nrm = vec2(-dir.y, dir.x);

    float zoom = max(pc.params.x, 1e-6);
    float thick_world = e.color.a / zoom;   // constant pixels on screen

    vec2 world = a + dir * (q.x * len) + nrm * (q.y * thick_world);
    gl_Position = pc.proj * pc.view * vec4(world, 0.0, 1.0);
    vColor = vec4(e.color.rgb, 1.0);
}
