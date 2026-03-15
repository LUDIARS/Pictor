// Pictor — Fullscreen Triangle Vertex Shader
// Generates a fullscreen triangle without any vertex buffer.
// Invoke with 3 vertices (gl_VertexIndex = 0, 1, 2).
// Covers the viewport with a single triangle (more efficient than a quad).

#version 450

layout(location = 0) out vec2 outUV;

void main() {
    // Generate fullscreen triangle vertices from vertex index
    //   idx=0: (-1, -1) uv=(0, 0)
    //   idx=1: ( 3, -1) uv=(2, 0)
    //   idx=2: (-1,  3) uv=(0, 2)
    // The triangle covers [-1,1]x[-1,1]; fragments outside are clipped.
    outUV = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(outUV * 2.0 - 1.0, 0.0, 1.0);
}
