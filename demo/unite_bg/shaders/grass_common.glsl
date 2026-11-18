// =============================================================================
// grass_common.glsl — 草システム (Scene 8) のバッファ定義
//   set 1: 0 = 配置データ (全インスタンス, セル順に並ぶ)
//          1 = 可視インスタンス番号 (LOD0 領域 | LOD1 領域)
//          2 = 間接描画コマンド x 2 (LOD0 / LOD1)
//          3 = 環境光 (毎フレーム 1 回だけ計算)
// =============================================================================
#ifndef UBG_GRASS_COMMON_GLSL
#define UBG_GRASS_COMMON_GLSL

struct GrassInstance {
    vec4 pos_scale;   // xyz = 根元の位置, w = スケール
    vec4 params;      // x = 向き(rad), y = 種類, z = 色ゆらぎ, w = 高さ[m]
};

layout(std430, set = 1, binding = 0) readonly buffer Instances { GrassInstance g_inst[]; };
layout(std430, set = 1, binding = 1) buffer Visible { uint g_visible[]; };
layout(std430, set = 1, binding = 2) buffer Indirect { uint g_cmd[]; };  // {vertexCount, instanceCount, firstVertex, firstInstance} x 2
layout(std430, set = 1, binding = 3) buffer EnvLight { vec4 g_env[4]; }; // 0 = 太陽光, 1 = 空(上), 2 = 空(水平)

// LOD ごとの形 (頂点数 = 本数 x (節数 x 2 + 1) x 3)
const int LOD0_BLADES = 5, LOD0_SEGS = 4;
const int LOD1_BLADES = 3, LOD1_SEGS = 1;

#endif
