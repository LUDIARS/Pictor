// Pictor — SHaRC ハイブリッド経路: 太陽シャドウマップ (深度のみ)
//
// 120fps 試算の (2): 近距離ピクセル精度シャドウレイ (BVH any-hit) を
// 4096^2 ortho 深度マップ (~1-2ms) に置き換える。 遠方セル太陽可視率は
// フルレイ経路 (A/B 比較用に残置) が引き続き使用。
// 頂点プリングは sharc_gbuffer.vert と同一規約。

#version 450

struct SharcTri {
    vec4 v0;
    vec4 v1;
    vec4 v2;
    vec4 uv01;
    vec4 uv2Pad;
};

layout(std430, set = 0, binding = 0) readonly buffer GbTris {
    SharcTri gb_tris[];
};

layout(push_constant) uniform GbPush {
    mat4 gbViewProj;    // 太陽の ortho VP
    vec4 gbCameraPos;   // 未使用 (G-buffer と push レイアウト共有)
};

void main() {
    uint triIdx = uint(gl_VertexIndex) / 3u;
    uint corner = uint(gl_VertexIndex) % 3u;
    SharcTri tri = gb_tris[triIdx];
    vec4 v = (corner == 0u) ? tri.v0 : (corner == 1u) ? tri.v1 : tri.v2;
    gl_Position = gbViewProj * vec4(v.xyz, 1.0);
}
