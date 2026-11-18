#version 450
// フルスクリーン三角形 (頂点バッファ不要)。uv は 0..1、左上原点。
layout(location = 0) out vec2 v_uv;
void main() {
    vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    v_uv = p;
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
