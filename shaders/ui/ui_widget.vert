// Pictor — UI Widget Vertex Shader
// push 定数の rect (px) と viewport から、 vertexIndex 0..3 で
// triangle-strip の quad を NDC に生成する。 頂点バッファ無し。

#version 450

layout(location = 0) out vec2 outUV;       // 0..1
layout(location = 1) out vec2 outLocalPx;  // rect ローカル px (SDF 用)

layout(push_constant) uniform PC {
    vec4 rect;       // x, y, w, h  (画面 px, 左上原点)
    vec4 color;      // rgba
    vec4 params;     // corner_radius, mode, opacity, _pad
    vec4 nine;       // 9-slice insets l, t, r, b (px)
    vec4 viewport;   // viewport_w, viewport_h, _pad, _pad
};

void main() {
    // triangle-strip: 0=(0,0) 1=(1,0) 2=(0,1) 3=(1,1)
    vec2 corner = vec2(float(gl_VertexIndex & 1), float((gl_VertexIndex >> 1) & 1));
    outUV       = corner;
    outLocalPx  = corner * rect.zw;

    vec2 px = rect.xy + corner * rect.zw;        // 画面 px
    vec2 ndc = (px / viewport.xy) * 2.0 - 1.0;   // Vulkan NDC (y 下向き)
    gl_Position = vec4(ndc, 0.0, 1.0);
}
