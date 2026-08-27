#version 450

/// 影絵デモ — 月光ゴッドレイ (フルスクリーン三角形、頂点バッファなし)。
/// NDC をそのままフラグメントへ渡し、レイ再構成は frag 側で行う。

layout(location = 0) out vec2 out_ndc;

void main() {
    // gl_VertexIndex 0/1/2 → (-1,-1) (3,-1) (-1,3) の 1 枚三角形
    vec2 ndc = vec2(float((gl_VertexIndex << 1) & 2) * 2.0 - 1.0,
                    float(gl_VertexIndex & 2) * 2.0 - 1.0);
    out_ndc = ndc;
    gl_Position = vec4(ndc, 0.0, 1.0);
}
