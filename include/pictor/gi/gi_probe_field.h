#pragma once

/// GIProbeField — irradiance probe grid の SH 構築 + 補間 (bake / realtime 共用)。
///
/// 静的ジオメトリ (`GISceneProxy`) に対して probe ごとの遮蔽レイをキャッシュ
/// し、 ライトが変わったら `relight()` で SH を再射影する — これが phase 1 の
/// 「リアルタイム GI」 の実体 (`spec/feature/gi-bake-realtime-design.md` §2.3)。
/// 静的シーン + 動的ライト + 動的オブジェクトというカジュアルゲームの典型
/// 構成をカバーする。 静的ジオメトリが変わったら build() し直す。
///
/// ライティングモデル (意図的な近似 — 全て決定的):
///   - 空 (ミス方向): sky_color を等方放射として射影。
///   - バウンス (1 次): キャッシュ済みサンプル方向のヒット面を Lambert 面
///     (albedo 一律 0.5、 法線 = probe 向き) とみなし、 ヒット点の太陽可視は
///     probe 自身の太陽可視で代用する (ヒット点ごとの追加レイを撃たない —
///     relight を probe 数 × (1 + 光源数) レイに抑えるため)。
///   - 直接光 (太陽 / 点光源の delta 射影) は **既定で含めない** —
///     マテリアルシェーダが解析的に直接光を評価するため、 probe に入れると
///     二重計上になる。 直接光まで probe に畳みたい consumer (完全 ambient
///     駆動の表現) のみ `BuildParams::include_direct` を立てる。

#include "pictor/core/types.h"
#include "pictor/gi/gi_lighting_system.h"
#include "pictor/gi/gi_scene_proxy.h"
#include "pictor/gi/gi_sh.h"

#include <vector>

namespace pictor {

/// probe grid 補間の共通実装 — `pos` を包む 8 probe の SH を trilinear 補間
/// して `out_sh` (float[36]) へ書く。 grid 外は端 probe へクランプ。
/// `sh_data` は probe_count × 36 float (grid は x → y → z の順で平坦化、
/// index = x + grid_x * (y + grid_y * z))。
/// `GILightingSystem` の CPU 補間経路と `GIProbeField::sample()` が共有する。
void sample_probe_grid(const GIProbeConfig& config, const float* sh_data,
                       const float3& pos, float out_sh[kSHFloatCount]);

class GIProbeField {
public:
    /// 構築パラメータ。 rays_per_probe は遮蔽キャッシュの方向数。
    struct BuildParams {
        uint32_t rays_per_probe = 64;
        float3   sky_color      = {0.35f, 0.45f, 0.65f}; ///< ミス方向の空放射
        float    sky_intensity  = 0.3f;
        float    bounce_albedo  = 0.5f;   ///< バウンス面の一律 albedo
        bool     include_direct = false;  ///< 直接光 delta 射影を含める (二重計上注意)
    };

    /// probe grid を構築する — 遮蔽レイをキャッシュし、 初回の relight まで行う。
    void build(const GIProbeConfig& config, const GISceneProxy& proxy,
               const DirectionalLight& sun, const std::vector<PointLight>& points,
               const BuildParams& params = {});

    /// ライトのみ変更時の再射影。 遮蔽キャッシュを再利用するため
    /// レイ数は probe 数 × (1 + 点光源数) に収まる (毎フレーム呼べる規模)。
    /// build() 前に呼んだ場合は何もしない。
    void relight(const DirectionalLight& sun, const std::vector<PointLight>& points);

    /// DDGI 風の予算更新 (phase 3) — round-robin カーソルから `probe_budget`
    /// 個の probe だけ遮蔽レイを撃ち直して再射影する。 数フレームかけて
    /// 全 probe が一巡し、 **動いたジオメトリ**に間接光が追従する。
    /// ホストは proxy の内容 (build() に渡したオブジェクト) を動的プールを
    /// 含めて更新してから呼ぶ (`GISceneProxy::build(static, dynamic)`)。
    /// 一巡あたりのコスト = probe_budget × rays_per_probe レイ / フレーム。
    void update_budgeted(const DirectionalLight& sun,
                         const std::vector<PointLight>& points,
                         uint32_t probe_budget);

    bool     built() const { return built_; }
    uint32_t probe_count() const { return probe_count_; }

    /// 全 probe の SH (probe_count × 36 float)。
    /// `GILightingSystem::upload_probe_data()` へそのまま渡せるレイアウト。
    const float* sh_data() const { return sh_.data(); }

    /// `pos` の補間 SH を得る (trilinear、 grid 外はクランプ)。
    void sample(const float3& pos, float out_sh[kSHFloatCount]) const;

    /// `pos` / `normal` の irradiance (RGB) を直接得る便宜関数。
    float3 sample_irradiance(const float3& pos, const float3& normal) const;

    const GIProbeConfig& config() const { return config_; }

private:
    float3 probe_position(uint32_t x, uint32_t y, uint32_t z) const;
    void   relight_probe(uint32_t index, const DirectionalLight& sun,
                         const std::vector<PointLight>& points);
    /// index 番 probe の遮蔽レイを撃ち直す (update_budgeted 用)。
    void   recast_probe(uint32_t index);

    GIProbeConfig config_{};
    BuildParams   params_{};
    uint32_t      probe_count_ = 0;
    bool          built_       = false;

    // 遮蔽キャッシュ (build で確保、 relight は読むだけ — DoD 規約):
    //   ray_hit_dist_ : probe × 方向。 ヒット距離、 ミスは < 0。
    //   sun_ray_ / point_ray_ 用の可視は relight 時に proxy を引くため、
    //   proxy への参照を build 時に保持する (非所有 — 呼び出し側が寿命保証)。
    std::vector<float> ray_hit_dist_;
    const GISceneProxy* proxy_ = nullptr;

    // SH 出力 (probe_count × 36)。
    std::vector<float> sh_;

    // update_budgeted の round-robin カーソル + レイ長キャッシュ。
    uint32_t update_cursor_ = 0;
    float    ray_length_    = 1.0f;
};

} // namespace pictor
