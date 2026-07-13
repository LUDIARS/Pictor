#pragma once

/// ポストプロセスの設定構造体。 実 Vulkan の実行は `PostProcessPipeline`
/// (host-driven) が担う。 旧 `PostProcessEffect` 抽象クラスと per-effect
/// クラス群 (BloomEffect 等) は実装パイプラインへ統合され撤去済み。

#include <string>
#include <cstdint>

namespace pictor {

/// HDR rendering configuration.
/// Controls the floating-point color buffer used before tone mapping.
///
/// auto_exposure = true のとき、 チェーンへ exposure_measure (輝度計測、
/// 1x1 viewport) + exposure_apply (シーンへ露出乗算) の 2 pass が挿入される
/// (phase 2 — history buffer を使った時間適応)。 適応は
/// `blend = 1 - exp(-delta_seconds * adaptation_rate)` で行い、
/// delta_seconds は `PostProcessPipeline::record()` が毎フレーム自動更新する。
struct HDRConfig {
    bool     enabled         = true;
    float    exposure        = 1.0f;     ///< Exposure multiplier (EV)
    float    gamma           = 2.2f;     ///< Display gamma
    float    white_point     = 4.0f;     ///< Whitepoint for extended-Reinhard
    float    min_luminance   = 0.001f;   ///< Minimum scene luminance (auto-exposure)
    float    max_luminance   = 10.0f;    ///< Maximum scene luminance (auto-exposure)
    bool     auto_exposure   = false;    ///< Enable auto-exposure (luminance adaptation)
    float    adaptation_rate = 1.5f;     ///< Auto-exposure adaptation speed (1/s)
    float    key             = 0.18f;    ///< Target middle-gray (exposure = key / avg_lum)
    /// 直近フレームの経過秒。 `PostProcessPipeline::record()` が詰める
    /// ランタイムフィールド — ホストが直接触る必要はない。
    float    delta_seconds   = 1.0f / 60.0f;
};

/// Tone-mapping operator selection.
enum class ToneMapOperator : uint8_t {
    ACES_FILMIC     = 0,  ///< Academy Color Encoding System filmic curve
    REINHARD        = 1,  ///< Simple Reinhard (luminance-based)
    REINHARD_EXT    = 2,  ///< Extended Reinhard with white point
    UNCHARTED2      = 3,  ///< Hable / Uncharted 2 filmic
    LINEAR_CLAMP    = 4,  ///< No tone mapping, just clamp
};

/// Bloom effect configuration.
///
/// mip_chain = true で progressive downsample/upsample 方式 (縮小ターゲット
/// チェーン、 広く柔らかい bloom) に切り替わる。 false (既定) は従来の
/// フルレス separable blur — 既存ホストの見た目を変えない opt-in。
/// 切替はチェーン構造変更なので `PostProcessPipeline::rebuild_chain()` 要。
struct BloomConfig {
    bool     enabled         = true;
    float    threshold       = 1.0f;     ///< Brightness threshold for bloom extraction
    float    soft_threshold  = 0.5f;     ///< Soft knee transition width
    float    intensity       = 0.8f;     ///< Bloom intensity multiplier
    float    radius          = 5.0f;     ///< Bloom blur radius (separable blur 時のみ)
    uint32_t mip_levels      = 5;        ///< mip チェーン段数 (2..6、 解像度で自動短縮)
    float    scatter         = 0.7f;     ///< upsample 時の下位 mip 寄与率
    bool     mip_chain       = false;    ///< progressive down/up チェーンを使う
};

/// Depth of Field configuration.
///
/// near_plane / far_plane (カメラのクリップ面、 ホストが設定するランタイム
/// データ) が両方 > 0 のとき、 シェーダは深度バッファ値を view 距離へ
/// 線形化してから focus_distance 等の world 距離と比較する (正しい挙動)。
/// 両方 0 (既定) は従来どおり生の深度値をそのまま距離として扱う
/// (レガシー互換 — 線形深度を書くホスト用)。
struct DepthOfFieldConfig {
    bool     enabled         = false;
    float    focus_distance  = 10.0f;    ///< Distance to the focal plane (world units)
    float    focus_range     = 5.0f;     ///< Sharp focus depth range
    float    bokeh_radius    = 4.0f;     ///< Maximum blur radius (pixels)
    float    near_start      = 0.0f;     ///< Near DoF start distance
    float    near_end        = 3.0f;     ///< Near DoF end distance (fully blurred)
    float    far_start       = 15.0f;    ///< Far DoF start distance
    float    far_end         = 50.0f;    ///< Far DoF end distance (fully blurred)
    uint32_t sample_count    = 16;       ///< Bokeh disc sample count
    float    near_plane      = 0.0f;     ///< カメラ near (0 = 線形化しない)
    float    far_plane       = 0.0f;     ///< カメラ far (0 = 線形化しない)
};

/// Gaussian blur configuration.
struct GaussianBlurConfig {
    bool     enabled         = false;
    float    sigma           = 2.0f;     ///< Standard deviation (kernel width)
    uint32_t kernel_size     = 9;        ///< Kernel tap count (odd, clamped 3..127)
    bool     separable       = true;     ///< Use separable 2-pass (H+V) blur
    float    intensity       = 1.0f;     ///< Output intensity multiplier
};

/// Tone-mapping configuration.
struct ToneMappingConfig {
    bool              enabled   = true;
    ToneMapOperator   op        = ToneMapOperator::ACES_FILMIC;
    float             exposure  = 1.0f;   ///< EV bias applied before tone mapping
    float             gamma     = 2.2f;   ///< Final gamma correction
    float             white_point = 4.0f; ///< Used with REINHARD_EXT
    float             saturation  = 1.0f; ///< Post-tonemap saturation (1.0 = unchanged)
};

/// Vignette configuration — darkens (or tints) screen edges.
struct VignetteConfig {
    bool   enabled   = true;
    float  intensity = 0.35f;          ///< 0 = none, 1 = strong edge darkening
    float  radius    = 0.75f;          ///< Normalized distance where falloff starts
    float  softness  = 0.45f;          ///< Falloff width (larger = smoother)
    float  color[3]  = {0.0f, 0.0f, 0.0f}; ///< Edge tint (usually black)
};

/// Color-grading configuration — applies a neutral LUT strip (PNG).
/// The strip is a `lut_size` tall by `lut_size*lut_size` wide RGBA8 image
/// (e.g. 16x256), the standard Unity / industry "neutral LUT" layout.
struct ColorGradingConfig {
    bool        enabled       = false;
    std::string lut_path;              ///< PNG LUT strip; empty disables the LUT
    float       lut_intensity = 1.0f;  ///< Blend 0..1 between original and graded
    uint32_t    lut_size      = 16;    ///< LUT cube edge (strip = size x size*size)
};

/// FXAA 3.11 (quality) — LDR (トーンマップ後) で走るスクリーンスペース AA。
/// 有効時は grade の出力が中間ターゲットへ差し替わる構造変更を伴う
/// (`build_post_process_chain()` — 切替は rebuild_chain() が必要)。
struct FXAAConfig {
    bool  enabled            = false;
    float edge_threshold     = 0.166f;   ///< 相対輝度コントラスト閾値
    float edge_threshold_min = 0.0833f;  ///< 暗部の絶対輝度閾値
    float subpix_quality     = 0.75f;    ///< サブピクセル AA 強度 0..1
};

/// SSAO (post-process 近似) — 深度差から遮蔽を推定しシーン色へ乗算する。
/// 直接光まで減衰する近似 (カジュアル用途向け)。ライティング項別の正確な
/// 適用 (間接光のみ減衰) は GI 側の責務
/// (`spec/feature/gi-bake-realtime-design.md` §3)。
struct SSAOPostConfig {
    bool     enabled      = false;
    uint32_t sample_count = 12;      ///< spiral kernel タップ数 (シェーダ側上限 32)
    float    radius       = 8.0f;    ///< サンプル半径 (ピクセル)
    float    bias         = 0.01f;   ///< 無視する深度差の下限 (自己遮蔽防止)
    float    range        = 0.05f;   ///< 遮蔽とみなす深度差の上限 (depth 単位)
    float    intensity    = 1.0f;    ///< 遮蔽の効き 0..1
    float    power        = 1.5f;    ///< AO カーブ (大きいほど締まる)
};

/// Motion Blur。 既定はカメラ再投影方式。 per_object = true (かつ
/// `PostProcessConfig::velocity.enabled`) で velocity buffer 方式に切り替わり、
/// 動くオブジェクトにもブラーが乗る (phase 3 — 切替は構造変更なので
/// rebuild_chain() 要。 velocity buffer 無効のまま per_object を立てた場合は
/// カメラ再投影で構築される)。
struct MotionBlurConfig {
    bool     enabled      = false;
    float    intensity    = 1.0f;    ///< ブラー長スケール
    uint32_t sample_count = 8;       ///< 速度ベクトルに沿うタップ数 (上限 16)
    float    max_velocity = 0.05f;   ///< NDC 単位の速度クランプ
    bool     per_object   = false;   ///< velocity buffer 方式 (要 velocity.enabled)
    /// prevVP * inverse(currVP) — ホストが毎フレーム更新し
    /// `refresh_post_process_chain()` が push constant へ詰め直す。
    /// レイアウトは float4x4::m と同一 (row-major 添字 m[row][col] を平坦化)。
    float    reproj_matrix[16] = {1.0f, 0.0f, 0.0f, 0.0f,
                                  0.0f, 1.0f, 0.0f, 0.0f,
                                  0.0f, 0.0f, 1.0f, 0.0f,
                                  0.0f, 0.0f, 0.0f, 1.0f};
    /// 初回フレーム / カメラワープ直後は false にする (そのフレームは素通し)。
    bool     matrix_valid = false;
};

/// TAA (Temporal Anti-Aliasing) — history buffer + カメラ再投影 + YCoCg
/// クランプ (phase 2)。 HDR 空間 (bloom / tonemap 前) で走る。
///
/// ホスト契約:
///   - 投影行列へ `taa_jitter()` のサブピクセルジッタを毎フレーム適用する。
///   - reproj_matrix (**ジッタ無しの** prevVP * inverse(currVP)) と
///     jitter_x/y (現フレームのジッタ、 ピクセル単位) を毎フレーム更新する。
///   - カメラカット / resize / rebuild_chain 後は history_valid = false
///     (1 フレーム素通しで history を再シード)。
///   - TAA と FXAA が両方 enabled のときは TAA が優先され FXAA は組み込み
///     チェーンから外れる (二重 AA 防止)。
struct TAAConfig {
    bool     enabled      = false;
    float    feedback_min = 0.85f;   ///< 動いている画素の history 混合率
    float    feedback_max = 0.95f;   ///< 静止画素の history 混合率
    float    reproj_matrix[16] = {1.0f, 0.0f, 0.0f, 0.0f,
                                  0.0f, 1.0f, 0.0f, 0.0f,
                                  0.0f, 0.0f, 1.0f, 0.0f,
                                  0.0f, 0.0f, 0.0f, 1.0f};
    bool     matrix_valid  = false;
    float    jitter_x      = 0.0f;   ///< 現フレームのジッタ (px、 unjitter 用)
    float    jitter_y      = 0.0f;
    bool     history_valid = false;  ///< false = このフレームは素通し + 再シード
    /// velocity buffer で再投影する (要 `PostProcessConfig::velocity.enabled`、
    /// phase 3)。 動体の history 追跡が正確になる。 reproj_matrix は不要になる
    /// (matrix_valid の代わりに history_valid だけで駆動)。
    bool     use_velocity  = false;
};

/// TAA 用カメラジッタ (Halton 2,3 列、 8 フレーム周期)。 ピクセル単位の
/// オフセットを返す — ホストは投影行列の [2][0]/[2][1] (列優先) へ
/// `2*jx/width` / `2*jy/height` を加算して適用する。
inline void taa_jitter(uint32_t frame_index, float& out_x, float& out_y) {
    // Halton(2) / Halton(3) の先頭 8 要素 (中心化済み、 ±0.5px)。
    static constexpr float kX[8] = { 0.0f, -0.25f,  0.25f, -0.375f,
                                     0.125f, -0.125f,  0.375f, -0.4375f};
    static constexpr float kY[8] = {-0.33333f,  0.33333f, -0.11111f,  0.22222f,
                                    -0.22222f,  0.11111f,  0.44444f, -0.44444f};
    const uint32_t i = frame_index & 7u;
    out_x = kX[i];
    out_y = kY[i];
}

/// SSR (Screen-Space Reflections) — 深度再構築版 (法線は深度勾配から復元、
/// phase 2)。 標準的な透視投影 (Vulkan z ∈ [0,1]、 非 reversed-Z) を仮定する。
///
/// ホスト契約: proj_xx / proj_yy (投影行列の [0][0] / [1][1])、 near / far を
/// カメラと一致させて設定する (view 空間再構築に使う)。
struct SSRConfig {
    bool     enabled     = false;
    float    intensity   = 0.6f;    ///< 反射の混合率 0..1
    uint32_t max_steps   = 32;      ///< レイマーチ最大ステップ (上限 64)
    float    stride_px   = 8.0f;    ///< 1 ステップのスクリーン距離 (px)
    float    thickness   = 0.5f;    ///< ヒット判定の view-space 厚み
    float    proj_xx     = 1.0f;    ///< 投影行列 [0][0]
    float    proj_yy     = 1.0f;    ///< 投影行列 [1][1]
    float    near_plane  = 0.1f;
    float    far_plane   = 1000.0f;
};

/// 色収差 — 画面端で RGB を放射方向にずらす (grade pass へ統合)。
struct ChromaticAberrationConfig {
    bool  enabled      = false;
    float intensity    = 0.5f;   ///< 最大ずらし量 (画面幅比のスケール 0..1)
    float start_radius = 0.3f;   ///< 効き始めの正規化半径 (中心 0 .. 角 ~1)
};

/// フィルムグレイン (grade pass へ統合)。 seed は毎フレーム進める
/// (`refresh_post_process_chain()` が config の値を転記するだけ —
/// 進行はホスト責務。固定なら静止ノイズになる)。
struct FilmGrainConfig {
    bool  enabled   = false;
    float intensity = 0.35f;
    float response  = 0.8f;   ///< 輝度依存 (明部でグレインを弱める) 0..1
    float seed      = 0.0f;
};

/// velocity buffer (phase 3、 opt-in) — scene render pass を MRT 化し、
/// attachment 1 に RG16F のスクリーン速度 (NDC 差分 × 0.5 = UV 差分) を持つ。
///
/// ホスト契約:
///   - シーンシェーダが location 1 へ velocity を書く
///     (`shaders/velocity.glsl` の pictor_encode_velocity() 参照)。
///   - scene pass の clearValueCount は 3 (color / velocity / depth)。
///     velocity のクリア値は {0,0,0,0} (静止)。
/// 有効/無効はチェーン構造 + scene render pass の変更なので
/// rebuild_chain() (または再初期化) を要する。
struct VelocityBufferConfig {
    bool enabled = false;
};

/// 疑似レンズフレア (ghost + halo、 phase 3)。 bloom 抽出結果を反転 UV で
/// 重ねる標準的なスクリーンスペース近似 — 光源データを要求しない。
struct LensFlareConfig {
    bool     enabled        = false;
    float    intensity      = 0.4f;   ///< フレア全体の強さ
    uint32_t ghost_count    = 4;      ///< ghost 数 (上限 8)
    float    ghost_spacing  = 0.3f;   ///< ghost の間隔 (中心対称スケール)
    float    halo_radius    = 0.45f;  ///< halo リングの正規化半径
    float    halo_intensity = 0.3f;
};

/// 解析的 height fog + 太陽前方散乱 (phase 3)。 レイマーチ無しの閉形式
/// 積分によるカジュアル向け「volumetric 風」フォグ — シャドウボリュームは
/// 評価しない。 カメラ基底 / 太陽はランタイムデータ (ホストが毎フレーム更新)。
struct VolumetricFogConfig {
    bool  enabled        = false;
    float color[3]       = {0.6f, 0.7f, 0.85f};
    float density        = 0.02f;   ///< 基準高度での消散係数
    float height_falloff = 0.1f;    ///< 高度による密度減衰 (大きいほど薄く)
    float base_height    = 0.0f;    ///< 密度基準の world 高度
    float start_distance = 5.0f;    ///< フォグ開始距離
    float phase_g        = 0.6f;    ///< 前方散乱の異方性 (0 = 等方)
    float sun_scatter    = 1.0f;    ///< 太陽散乱の寄与

    // ---- runtime (ホストが毎フレーム更新) ----
    float camera_pos[3]     = {0.0f, 0.0f, 0.0f};
    float camera_forward[3] = {0.0f, 0.0f, 1.0f};
    float camera_right[3]   = {1.0f, 0.0f, 0.0f};
    float camera_up[3]      = {0.0f, 1.0f, 0.0f};
    float tan_half_fov_x    = 1.0f;
    float tan_half_fov_y    = 1.0f;
    float sun_dir[3]        = {0.0f, -1.0f, 0.0f};
    float sun_color[3]      = {1.0f, 1.0f, 1.0f};   ///< intensity 込み
    float near_plane        = 0.1f;
    float far_plane         = 1000.0f;
    bool  camera_valid      = false;   ///< false = 素通し (未設定フレーム)
};

/// SSGI (screen-space GI、 phase 3) — 画面内の明るい面からの 1 次バウンスを
/// half-res で集め、 history buffer の時間フィルタでノイズを均して加算する。
/// 画面外の光は拾えない — probe GI (gi-bake-realtime-design.md) の補助。
struct SSGIConfig {
    bool     enabled      = false;
    float    intensity    = 1.0f;
    uint32_t sample_count = 8;       ///< spiral タップ数 (上限 16)
    float    radius_px    = 32.0f;   ///< 集光半径 (フル解像度 px)
    float    feedback     = 0.8f;    ///< 時間フィルタの history 混合率
    // ---- runtime (SSR と同じ透視パラメータ、 ホスト供給) ----
    float    proj_xx      = 1.0f;
    float    proj_yy      = 1.0f;
    float    near_plane   = 0.1f;
    float    far_plane    = 1000.0f;
};

/// Aggregated post-process stack configuration.
/// Determines which effects are active and their parameters.
struct PostProcessConfig {
    HDRConfig                 hdr;
    BloomConfig               bloom;
    DepthOfFieldConfig        depth_of_field;
    GaussianBlurConfig        gaussian_blur;
    ToneMappingConfig         tone_mapping;
    VignetteConfig            vignette;
    ColorGradingConfig        color_grading;
    SSAOPostConfig            ssao;
    MotionBlurConfig          motion_blur;
    FXAAConfig                fxaa;
    ChromaticAberrationConfig chromatic_aberration;
    FilmGrainConfig           film_grain;
    TAAConfig                 taa;
    SSRConfig                 ssr;
    VelocityBufferConfig      velocity;
    LensFlareConfig           lens_flare;
    VolumetricFogConfig       volumetric_fog;
    SSGIConfig                ssgi;
};

} // namespace pictor
