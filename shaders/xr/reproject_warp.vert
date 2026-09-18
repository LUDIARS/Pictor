#version 450

// 描画済みの画 (色 + 深度) を別の視点へ写す「格子メッシュの変形」。
// 頂点バッファは使わず、 gl_VertexIndex から格子上の位置を決める。 各頂点は元の
// 深度を読んで元の NDC 座標を作り、 行列 1 つで写し先のクリップ座標へ移す。
// 遮蔽はラスタライザの深度テストが解決する。

layout(push_constant) uniform Push {
    mat4  dst_clip_from_src_ndc;
    ivec2 grid_cells;      // 格子のマス数 (横, 縦)
    vec2  src_size;        // 元の画の解像度 (画素)
    float min_texel_rate;  // これ未満に引き伸ばされた三角形は穴として捨てる
    float depth_bias;      // 写した画素を少し手前へ置き、 穴埋めの再描画を弾く
} pc;

layout(set = 0, binding = 1) uniform sampler2D src_depth;

layout(location = 0) out vec2 v_src_uv;

const ivec2 kCorner[6] = ivec2[6](
    ivec2(0, 0), ivec2(1, 0), ivec2(0, 1),
    ivec2(0, 1), ivec2(1, 0), ivec2(1, 1));

void main() {
    // @implements SPEC-PC-XR-DELTA-RENDERING
    int   cell = gl_VertexIndex / 6;
    ivec2 at   = ivec2(cell % pc.grid_cells.x, cell / pc.grid_cells.x)
               + kCorner[gl_VertexIndex % 6];
    vec2  uv   = vec2(at) / vec2(pc.grid_cells);

    float depth = textureLod(src_depth, uv, 0.0).r;
    vec4  clip  = pc.dst_clip_from_src_ndc * vec4(uv * 2.0 - 1.0, depth, 1.0);
    clip.z -= pc.depth_bias * clip.w;

    gl_Position = clip;
    v_src_uv    = uv;
}
