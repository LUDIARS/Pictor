// Pictor — SHaRC 拡張: present 用フルスクリーン三角形
//
// 頂点バッファなし。 gl_VertexIndex 0..2 から NDC を張る定型。
// フラグメント側が output SSBO を直読みするため UV のみ渡す。

#version 450

layout(location = 0) out vec2 outUv;

void main() {
    // (0,0) (2,0) (0,2) の UV → 画面を覆う 1 枚三角形
    outUv = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(outUv * 2.0 - 1.0, 0.0, 1.0);
}
