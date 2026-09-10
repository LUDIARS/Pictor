#pragma once

// Backend-independent SHaRC input contract. No GPU API or game types.
#include "pictor/gi/sharc_types.h"

#include <cstdint>

namespace pictor {

/// 初期化パラメータ (途中変更は shutdown → 再 init)。
struct SharcConfig {
    uint32_t table_size     = 1u << 16;  ///< ハッシュスロット数 (2^n 必須)
    uint32_t max_rays       = 320 * 180; ///< march / resolve の最大レイ数
    uint32_t max_hits       = 1u << 20;  ///< ヒット列容量 (DX12 ポートのみ)
    uint32_t max_lights     = 64;        ///< ライトバッファ容量
    float    base_cell_size = 0.25f;     ///< level 0 セル一辺 (m)
    uint32_t level_count    = 8;
    float    ema_alpha      = 0.05f;     ///< 時間平均係数
    float    level_bias     = 0.5f;      ///< 距離→レベル選択バイアス
    float    sss_mfp_scale  = 3.0f;      ///< MFP→レベル選択倍率
    uint32_t max_ray_steps  = 64;        ///< march の最大セルステップ
    uint32_t stale_frames   = 240;       ///< エビクション閾値
    float    hit_epsilon    = 1e-3f;
};

/// GPU 入力レイ (shaders/sharc/sharc_march.comp の SharcRay と同一)。
struct SharcRayGpu {
    float origin_tmin[4];   ///< xyz = 原点, w = tMin
    float dir_tmax[4];      ///< xyz = 方向 (正規化), w = tMax
};

/// GPU シェーディング要求 (sharc_resolve.comp の SharcShadeRequest と同一)。
struct SharcShadeRequestGpu {
    float pos_rough[4];     ///< xyz = ヒット点, w = roughness
    float normal_mfp[4];    ///< xyz = 法線, w = SSS MFP (0 = 無効)
    float albedo_view[4];   ///< rgb = albedo, a = 予約
    float view_dir[4];      ///< xyz = ヒット点→カメラ, w = 予約
};

/// GPU ライト (sharc_update.comp の SharcLight と同一)。
struct SharcLightGpu {
    float pos_radius[4];       ///< xyz = 位置, w = 半径
    float color_intensity[4];  ///< rgb = 色, a = 強度
};

/// GPU シーン (hit パス) の一括アップロード内容。 全てフラット DoD 配列で、
/// デモ側の BVH / メッシュをレイアウト変換なしに近い形で転写する。
struct SharcSceneUpload {
    const SharcBvhNodeGpu* nodes = nullptr;
    uint32_t node_count = 0;
    const SharcTriGpu* tris = nullptr;        ///< 葉順に並べ替え済み
    uint32_t tri_count = 0;
    const uint32_t* tri_materials = nullptr;  ///< [tri_count]
    const SharcMaterialGpu* materials = nullptr;
    uint32_t material_count = 0;
    /// アルベドテクスチャ配列 (RGBA8 sRGB、 [layer][size*size*4] 連結)。
    /// nullptr / 0 レイヤなら 1x1 白ダミーを維持する。
    const uint8_t* atlas_pixels = nullptr;
    uint32_t atlas_size   = 0;
    uint32_t atlas_layers = 0;
};

} // namespace pictor
