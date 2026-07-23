#pragma once

/// SHaRC 拡張ライティングキャッシュのレイアウト定義 (CPU 側ミラー)。
///
/// spec: `pictor-sharc-ext-design.md` (SHaRC拡張 ライティングキャッシュ設計)。
/// ワールド空間ハッシュセルに 2 層表現を載せる:
///   層1 モーメント M0/M1 (SSS・拡散, 16B) / 層2 vMF/SG ローブ (鏡面, 12B×N)
///   層3 メタデータ (16B)。 既定 4 ローブで 1 セル = 80B。
/// GPU 側 (`shaders/sharc/sharc_common.glsl`) とバイト互換必須 —
/// 変更時は両方を同時に更新すること。
///
/// 全て純関数 / 定数 (状態なし・決定的) — headless テスト対象。

#include "pictor/core/types.h"

#include <cmath>
#include <cstdint>

namespace pictor {

// ============================================================
// セルレイアウト (uint32 単位)
// ============================================================

/// 層2 ローブ本数 (コンパイル時固定, GLSL 側 SHARC_LOBE_COUNT と一致)。
constexpr uint32_t kSharcLobeCount = 4;

/// 層1 モーメント (uvec4)。
constexpr uint32_t kSharcCellUintsMoments = 4;
/// 層2 ローブ (3 uint × 本数)。
constexpr uint32_t kSharcCellUintsLobes = kSharcLobeCount * 3;
/// 層3 メタデータ (uvec4)。
constexpr uint32_t kSharcCellUintsMeta = 4;
/// セル総量 (uint 数)。 既定 20 uint = 80B。
constexpr uint32_t kSharcCellUints =
    kSharcCellUintsMoments + kSharcCellUintsLobes + kSharcCellUintsMeta;
/// セルサイズ (バイト)。
constexpr uint32_t kSharcCellBytes = kSharcCellUints * 4;

constexpr uint32_t kSharcCellOffsetMoments = 0;
constexpr uint32_t kSharcCellOffsetLobes = kSharcCellUintsMoments;
constexpr uint32_t kSharcCellOffsetMeta =
    kSharcCellUintsMoments + kSharcCellUintsLobes;

/// ハッシュ線形走査の上限 (GLSL SHARC_PROBE_LIMIT と一致)。
constexpr uint32_t kSharcProbeLimit = 16;
/// 無効スロット。
constexpr uint32_t kSharcSlotInvalid = 0xFFFFFFFFu;

// ============================================================
// SharcParams UBO ミラー (std140 互換配置)
// ============================================================

struct alignas(16) SharcParamsGpu {
    float camera_pos[4];       ///< xyz = カメラ位置
    float base_cell_size;      ///< level 0 のセル一辺 (m)
    uint32_t level_count;      ///< ハッシュ階層レベル数
    uint32_t table_size;       ///< ハッシュテーブルスロット数 (2^n 必須)
    uint32_t frame_index;      ///< EMA / エビクション用フレーム番号
    float ema_alpha;           ///< 時間平均係数 (0..1)
    float level_bias;          ///< 距離→レベル選択バイアス
    float sss_mfp_scale;       ///< MFP→レベル選択倍率
    uint32_t max_ray_steps;    ///< march 最大セルステップ
    uint32_t light_count;
    uint32_t ray_count;
    uint32_t stale_frames;     ///< エビクション閾値 (フレーム)
    float hit_epsilon;         ///< t 区間の数値マージン
    uint32_t scene_tri_count;  ///< GPU シーンの三角形数 (0 = hit パス無効)
    uint32_t scene_flags;      ///< kSharcSceneMesh | kSharcSceneFloor
    float floor_y;             ///< 解析床の高さ
    float scene_ray_far;       ///< hit パスの初期 tMax
    float cam_fwd[4];          ///< xyz = 前方, w = fovScale
    float cam_right[4];        ///< xyz = 右, w = アスペクト比
    float cam_up[4];           ///< xyz = 上, w = レンダリング幅 (float)
};
static_assert(sizeof(SharcParamsGpu) == 128, "GLSL SharcParams と一致させる");

/// scene_flags (GLSL SHARC_SCENE_* と一致)。
constexpr uint32_t kSharcSceneMesh   = 1u;
constexpr uint32_t kSharcSceneFloor  = 2u;
constexpr uint32_t kSharcSceneAlbedo = 4u;   ///< アルベド素通し (検証用)

// ============================================================
// GPU シーン (hit パス) のレイアウト — sharc_hit.comp と一致
// ============================================================

/// BVH ノード (32B)。 左子 = 自ノード + 1、 right/leaf-start を left に格納。
struct SharcBvhNodeGpu {
    float bmin[3];
    uint32_t left;      ///< 内部: right 子 index / 葉: 三角形開始
    float bmax[3];
    uint32_t count;     ///< 0 = 内部 / >0 = 葉の三角形数
};
static_assert(sizeof(SharcBvhNodeGpu) == 32, "GLSL SharcBvhNode と一致させる");

/// 三角形 (80B)。 n = oct32 頂点法線、 uv = コーナー UV (texture2DArray
/// を REPEAT サンプルするため生値、 [0,1] 超のタイルも保持する)。
struct SharcTriGpu {
    float v0[3]; uint32_t n0;
    float v1[3]; uint32_t n1;
    float v2[3]; uint32_t n2;
    float uv0[2];
    float uv1[2];
    float uv2[2];
    uint32_t pad0, pad1;
};
static_assert(sizeof(SharcTriGpu) == 80, "GLSL SharcTri と一致させる");

/// マテリアル (32B)。 atlas_layer_plus1 は 0 = テクスチャなし、
/// N > 0 = texture2DArray のレイヤ N-1 (float 格納、 ≤256 なので厳密)。
struct SharcMaterialGpu {
    float albedo[3];
    float roughness;
    float mfp;
    float atlas_layer_plus1;
    float pad[2];
};
static_assert(sizeof(SharcMaterialGpu) == 32, "GLSL SharcMaterial と一致させる");

/// 単位ベクトル → oct32 (GLSL sharcOct32Encode と同一)。
inline uint32_t sharc_oct32_encode(float nx, float ny, float nz) {
    const float len = std::fabs(nx) + std::fabs(ny) + std::fabs(nz);
    const float inv = (len > 1e-12f) ? 1.0f / len : 0.0f;
    float px = nx * inv, py = ny * inv;
    if (nz < 0.0f) {
        const float ox = (1.0f - std::fabs(py)) * (px >= 0.0f ? 1.0f : -1.0f);
        const float oy = (1.0f - std::fabs(px)) * (py >= 0.0f ? 1.0f : -1.0f);
        px = ox;
        py = oy;
    }
    const auto qx = static_cast<uint32_t>(
        (std::min)((std::max)(px * 32767.0f + 32767.5f, 0.0f), 65535.0f));
    const auto qy = static_cast<uint32_t>(
        (std::min)((std::max)(py * 32767.0f + 32767.5f, 0.0f), 65535.0f));
    return (qy << 16) | qx;
}

/// per-cell reservoir (uvec4): {lightIdx, wSum(f32), M(f32), targetPdf(f32)}。
constexpr uint32_t kSharcReservoirUints = 4;

/// ヒットレコード {rayIdx, slot, t0, t1} (std430)。
constexpr uint32_t kSharcHitRecordBytes = 16;

// ============================================================
// レベル / グリッド (GLSL と同一式)
// ============================================================

/// level のセル一辺 (m)。
inline float sharc_cell_size(float base_cell_size, uint32_t level) {
    return base_cell_size * std::exp2(static_cast<float>(level));
}

/// カメラ距離ベースのレベル選択。
inline uint32_t sharc_level_from_distance(float dist, float base_cell_size,
                                          float level_bias,
                                          uint32_t level_count) {
    const float l =
        std::log2((std::max)(dist * level_bias / base_cell_size, 1.0f));
    const auto li = static_cast<uint32_t>(l);
    return (std::min)(li, level_count - 1);
}

/// MFP ベースのレベル選択 (SSS 用, spec §2)。
inline uint32_t sharc_level_from_mfp(float mfp, float base_cell_size,
                                     float sss_mfp_scale,
                                     uint32_t level_count) {
    const float radius = (std::max)(mfp * sss_mfp_scale, base_cell_size);
    const float l = std::log2(radius / base_cell_size);
    const auto li = static_cast<uint32_t>(std::lround(l));
    return (std::min)(li, level_count - 1);
}

// ============================================================
// ハッシュ (GLSL と同一実装 — headless でテーブル挙動を検証するため)
// ============================================================

/// lowbias32 (Chris Wellons)。
inline uint32_t sharc_hash_uint(uint32_t x) {
    x ^= x >> 16; x *= 0x7FEB352Du;
    x ^= x >> 15; x *= 0x846CA68Bu;
    x ^= x >> 16;
    return x;
}

inline uint32_t sharc_hash_combine(uint32_t seed, uint32_t v) {
    return seed ^ (sharc_hash_uint(v) + 0x9E3779B9u + (seed << 6) +
                   (seed >> 2));
}

/// (grid, level) → 32bit 指紋 (0 は空スロット識別に予約)。
inline uint32_t sharc_fingerprint(int32_t gx, int32_t gy, int32_t gz,
                                  uint32_t level) {
    uint32_t h = sharc_hash_combine(0x811C9DC5u, static_cast<uint32_t>(gx));
    h = sharc_hash_combine(h, static_cast<uint32_t>(gy));
    h = sharc_hash_combine(h, static_cast<uint32_t>(gz));
    h = sharc_hash_combine(h, level);
    return (h == 0u) ? 1u : h;
}

/// (grid, level) → テーブル開始スロット。
inline uint32_t sharc_slot_hash(int32_t gx, int32_t gy, int32_t gz,
                                uint32_t level, uint32_t table_size) {
    uint32_t h = sharc_hash_combine(0x9747B28Cu, static_cast<uint32_t>(gx));
    h = sharc_hash_combine(h, static_cast<uint32_t>(gz));
    h = sharc_hash_combine(h, static_cast<uint32_t>(gy));
    h = sharc_hash_combine(h, level);
    return h & (table_size - 1);
}

// ============================================================
// グリッド座標逆引きパック (uvec2, GLSL sharcPackGridLevel と同一)
// ============================================================

struct SharcPackedGrid {
    uint32_t lo;
    uint32_t hi;
};

/// x/y/z 各 signed 20bit + level 4bit を 64bit に詰める。
inline SharcPackedGrid sharc_pack_grid_level(int32_t gx, int32_t gy,
                                             int32_t gz, uint32_t level) {
    const uint32_t bias = 1u << 19;
    const uint32_t ux = (static_cast<uint32_t>(gx) + bias) & 0xFFFFFu;
    const uint32_t uy = (static_cast<uint32_t>(gy) + bias) & 0xFFFFFu;
    const uint32_t uz = (static_cast<uint32_t>(gz) + bias) & 0xFFFFFu;
    SharcPackedGrid p;
    p.lo = ux | (uy << 20);
    p.hi = (uy >> 12) | (uz << 8) | ((level & 0xFu) << 28);
    return p;
}

inline void sharc_unpack_grid_level(SharcPackedGrid p, int32_t& gx,
                                    int32_t& gy, int32_t& gz,
                                    uint32_t& level) {
    const int32_t bias = 1 << 19;
    const uint32_t ux = p.lo & 0xFFFFFu;
    const uint32_t uy = (p.lo >> 20) | ((p.hi & 0xFFu) << 12);
    const uint32_t uz = (p.hi >> 8) & 0xFFFFFu;
    gx = static_cast<int32_t>(ux) - bias;
    gy = static_cast<int32_t>(uy) - bias;
    gz = static_cast<int32_t>(uz) - bias;
    level = (p.hi >> 28) & 0xFu;
}

}  // namespace pictor
