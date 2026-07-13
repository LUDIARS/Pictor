#include "pictor/postprocess/postprocess_chain.h"

#include "pictor/pipeline/pipeline_profile.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace pictor {

// 予約済み論理ターゲット名。
const char* const kPostProcessOutputTarget   = "__output__";
const char* const kPostProcessSceneTarget    = "__scene__";
const char* const kPostProcessLutTarget      = "__lut__";
const char* const kPostProcessDepthTarget    = "__depth__";
const char* const kPostProcessVelocityTarget = "__velocity__";
const char* const kPostProcessHistoryPrefix  = "__history:";

std::string history_input_name(std::string_view target) {
    std::string s(kPostProcessHistoryPrefix);
    s += target;
    s += "__";
    return s;
}

std::string parse_history_input(std::string_view name) {
    const std::string_view prefix(kPostProcessHistoryPrefix);
    if (name.size() <= prefix.size() + 2) return {};
    if (name.substr(0, prefix.size()) != prefix) return {};
    if (name.substr(name.size() - 2) != "__") return {};
    return std::string(name.substr(prefix.size(),
                                   name.size() - prefix.size() - 2));
}

namespace {

// 組み込みチェーンの中間ターゲット論理名 (ping-pong / 途中挿入 pass の出力)。
constexpr const char* kPingTarget     = "pp_ping";
constexpr const char* kPongTarget     = "pp_pong";
constexpr const char* kDofTarget      = "pp_dof";
constexpr const char* kSsaoTarget     = "pp_ssao";
constexpr const char* kSsrTarget      = "pp_ssr";
constexpr const char* kMBlurTarget    = "pp_mblur";
constexpr const char* kTaaTarget      = "pp_taa";
constexpr const char* kExposureTarget = "pp_exposure";
constexpr const char* kExposedTarget  = "pp_exposed";
constexpr const char* kLdrTarget      = "pp_ldr";
constexpr const char* kSsgiTarget     = "pp_ssgi";
constexpr const char* kSsgiOutTarget  = "pp_ssgi_out";
constexpr const char* kFogTarget      = "pp_fog";
constexpr const char* kFlareTarget    = "pp_flare";

// 旧 PostProcessPipeline の push constant 構造体と同一レイアウト。
// build_post_process_chain() / refresh_post_process_chain() はこの構造体を
// そのままバイト列へコピーするので、 旧実装と「ビット単位で同一」 になる。
struct ExtractPC { float threshold, soft_threshold, pad0, pad1; };
struct BlurPC    { float dir_x, dir_y, radius, pad0; };
struct GradePC {
    float    bloom_intensity;
    uint32_t tonemap_op;
    float    exposure, gamma, white_point, saturation;
    float    vignette_intensity, vignette_radius, vignette_softness;
    float    lut_intensity, lut_size;
    float    vig_r, vig_g, vig_b;
    // 2026-07 拡張 (spec/feature/postprocess-effects-design.md §2.1):
    // 色収差 + フィルムグレインは独立 pass を増やさず grade へ統合する。
    // 既存 14 フィールドの前方レイアウトは不変 — 旧シェーダとの
    // push constant 互換を保ったまま末尾追加のみ。
    float    ca_intensity, ca_start_radius;
    float    grain_intensity, grain_response, grain_seed;
};
// dof.frag の push constant (shaders/postprocess/dof.frag と同一レイアウト)。
// 旧 pad0/pad1 は near/far クリップ面に転用 (0 = 深度線形化なし = 旧挙動)。
struct DofPC {
    float    focus_distance, focus_range, bokeh_radius;
    float    near_start, near_end, far_start, far_end;
    uint32_t sample_count;
    float    texel_x, texel_y, near_plane, far_plane;
};
// ssao_apply.frag の push constant。
struct SsaoPC {
    float    radius_px, bias, range, intensity;
    float    power, texel_x, texel_y;
    uint32_t sample_count;
};
// motion_blur.frag の push constant。 reproj = prevVP * inverse(currVP) を
// 1 枚に畳むことで push 128B 制限内に収める (行列 2 枚は入らない)。
struct MotionBlurPC {
    float    reproj[16];
    float    intensity, max_velocity;
    uint32_t sample_count;
    uint32_t valid;          // 0 = 素通し (初回フレーム / カメラワープ)
};
// fxaa.frag の push constant。
struct FxaaPC {
    float texel_x, texel_y;
    float edge_threshold, edge_threshold_min;
    float subpix_quality, pad0, pad1, pad2;
};
// taa.frag の push constant。
struct TaaPC {
    float    reproj[16];
    float    feedback_min, feedback_max;
    float    jitter_x, jitter_y;      // px
    float    texel_x, texel_y;
    uint32_t valid;                   // 0 = 素通し + history 再シード
    uint32_t pad0;
};
// ssr.frag の push constant。
struct SsrPC {
    float    proj_xx, proj_yy, near_plane, far_plane;
    float    intensity, stride_px, thickness;
    uint32_t max_steps;
    float    texel_x, texel_y, pad0, pad1;
};
// exposure_measure.frag / exposure_apply.frag の push constant。
struct ExposureMeasurePC {
    float min_lum, max_lum, blend, pad0;
};
struct ExposureApplyPC {
    float key, pad0, pad1, pad2;      // key <= 0 で素通し
};
// bloom_down.frag / bloom_up.frag の push constant (mip チェーン)。
struct BloomDownPC {
    float texel_x, texel_y, pad0, pad1;   // texel = ソース mip のもの
};
struct BloomUpPC {
    float texel_x, texel_y, scatter, pad0; // texel = 下位 mip のもの
};
// motion_blur_velocity.frag の push constant (velocity buffer 方式 —
// 再投影行列が不要になる)。
struct MotionBlurVelocityPC {
    float    intensity, max_velocity;
    uint32_t sample_count, valid;
};
// taa_velocity.frag の push constant (velocity buffer 方式)。
struct TaaVelocityPC {
    float    feedback_min, feedback_max;
    float    jitter_x, jitter_y;
    float    texel_x, texel_y;
    uint32_t valid, pad0;
};
// lens_flare.frag の push constant。
struct LensFlarePC {
    float    intensity, ghost_spacing, halo_radius, halo_intensity;
    uint32_t ghost_count;
    float    texel_x, texel_y, pad0;
};
// volumetric_fog.frag の push constant (128B ちょうど — push 上限)。
struct FogPC {
    float    cam_pos[3];   float density;
    float    cam_fwd[3];   float height_falloff;
    float    cam_right[3]; float base_height;
    float    cam_up[3];    float start_dist;
    float    sun_dir[3];   float phase_g;
    float    sun_color[3]; float sun_scatter;
    float    fog_color[3]; float near_plane;
    float    far_plane, tan_x, tan_y;
    uint32_t valid;
};
static_assert(sizeof(FogPC) == 128, "FogPC must fit the 128B push limit");
// ssgi_gather.frag / ssgi_apply.frag の push constant。
struct SsgiGatherPC {
    float    proj_xx, proj_yy, near_plane, far_plane;
    float    radius_px, feedback, texel_x, texel_y;
    uint32_t sample_count;
    float    pad0, pad1, pad2;
};
struct SsgiApplyPC {
    float intensity, pad0, pad1, pad2;
};

// 構造体を push_data バイト列へ詰める。
template <typename T>
void store_pc(std::vector<uint8_t>& out, const T& pc) {
    out.resize(sizeof(T));
    std::memcpy(out.data(), &pc, sizeof(T));
}

// --- 旧 record() と同一の push 値を組む -----------------------------------

ExtractPC make_extract_pc(const PostProcessConfig& cfg) {
    const auto& bloom = cfg.bloom;
    // disabled bloom → 異常に高い threshold で何も抽出しない (旧実装と同一)。
    return ExtractPC{bloom.enabled ? bloom.threshold : 1.0e9f,
                     bloom.soft_threshold, 0.0f, 0.0f};
}

BlurPC make_blur_h_pc(const PostProcessConfig& cfg, uint32_t w) {
    const float radius = cfg.bloom.enabled ? cfg.bloom.radius : 1.0f;
    return BlurPC{1.0f / static_cast<float>(w ? w : 1), 0.0f, radius, 0.0f};
}

BlurPC make_blur_v_pc(const PostProcessConfig& cfg, uint32_t h) {
    const float radius = cfg.bloom.enabled ? cfg.bloom.radius : 1.0f;
    return BlurPC{0.0f, 1.0f / static_cast<float>(h ? h : 1), radius, 0.0f};
}

/// mip チェーン段数 — config を [2,6] にクランプし、 最小 mip が 8px を
/// 下回らないよう解像度で自動短縮する。
uint32_t bloom_mip_count(const PostProcessConfig& cfg, uint32_t w, uint32_t h) {
    uint32_t mips = std::clamp(cfg.bloom.mip_levels, 2u, 6u);
    const uint32_t min_dim = std::max(std::min(w, h), 16u);
    while (mips > 2 && (min_dim >> mips) < 8u) --mips;
    return mips;
}

BloomDownPC make_bloom_down_pc(uint32_t level, uint32_t w, uint32_t h) {
    // down_k のソースは d(k-1) (divisor 2^k)。
    const float sx = static_cast<float>(1u << level);
    BloomDownPC pc{};
    pc.texel_x = sx / static_cast<float>(w ? w : 1);
    pc.texel_y = sx / static_cast<float>(h ? h : 1);
    return pc;
}

BloomUpPC make_bloom_up_pc(const PostProcessConfig& cfg, uint32_t level,
                           uint32_t w, uint32_t h) {
    // up_k の下位入力は divisor 2^(k+2) (u(k+1) または d(M-1))。
    const float sx = static_cast<float>(1u << (level + 2));
    BloomUpPC pc{};
    pc.texel_x = sx / static_cast<float>(w ? w : 1);
    pc.texel_y = sx / static_cast<float>(h ? h : 1);
    pc.scatter = cfg.bloom.enabled ? cfg.bloom.scatter : 0.0f;
    return pc;
}

DofPC make_dof_pc(const PostProcessConfig& cfg, uint32_t w, uint32_t h) {
    const auto& dof = cfg.depth_of_field;
    DofPC pc{};
    pc.focus_distance = dof.focus_distance;
    pc.focus_range    = dof.focus_range;
    // disabled → bokeh 半径 0 で恒等へ縮退 (シェーダは CoC < 0.5 で素通し)。
    pc.bokeh_radius   = dof.enabled ? dof.bokeh_radius : 0.0f;
    pc.near_start     = dof.near_start;
    pc.near_end       = dof.near_end;
    pc.far_start      = dof.far_start;
    pc.far_end        = dof.far_end;
    pc.sample_count   = dof.sample_count;
    pc.texel_x        = 1.0f / static_cast<float>(w ? w : 1);
    pc.texel_y        = 1.0f / static_cast<float>(h ? h : 1);
    pc.near_plane     = dof.near_plane;
    pc.far_plane      = dof.far_plane;
    return pc;
}

SsaoPC make_ssao_pc(const PostProcessConfig& cfg, uint32_t w, uint32_t h) {
    const auto& ao = cfg.ssao;
    SsaoPC pc{};
    pc.radius_px    = ao.radius;
    pc.bias         = ao.bias;
    pc.range        = ao.range;
    // disabled → intensity 0 で恒等へ縮退 (pass 構造は維持、 refresh 用)。
    pc.intensity    = ao.enabled ? ao.intensity : 0.0f;
    pc.power        = ao.power;
    pc.texel_x      = 1.0f / static_cast<float>(w ? w : 1);
    pc.texel_y      = 1.0f / static_cast<float>(h ? h : 1);
    pc.sample_count = ao.sample_count;
    return pc;
}

MotionBlurPC make_motion_blur_pc(const PostProcessConfig& cfg) {
    const auto& mb = cfg.motion_blur;
    MotionBlurPC pc{};
    std::memcpy(pc.reproj, mb.reproj_matrix, sizeof(pc.reproj));
    pc.intensity    = mb.intensity;
    pc.max_velocity = mb.max_velocity;
    pc.sample_count = mb.sample_count;
    // disabled / 行列未確定 → valid 0 で素通し (恒等縮退)。
    pc.valid        = (mb.enabled && mb.matrix_valid) ? 1u : 0u;
    return pc;
}

FxaaPC make_fxaa_pc(const PostProcessConfig& cfg, uint32_t w, uint32_t h) {
    const auto& fx = cfg.fxaa;
    FxaaPC pc{};
    pc.texel_x = 1.0f / static_cast<float>(w ? w : 1);
    pc.texel_y = 1.0f / static_cast<float>(h ? h : 1);
    // disabled → コントラスト閾値を超えるエッジが存在しなくなり素通し。
    pc.edge_threshold     = fx.enabled ? fx.edge_threshold : 1.0e9f;
    pc.edge_threshold_min = fx.enabled ? fx.edge_threshold_min : 1.0e9f;
    pc.subpix_quality     = fx.enabled ? fx.subpix_quality : 0.0f;
    return pc;
}

TaaPC make_taa_pc(const PostProcessConfig& cfg, uint32_t w, uint32_t h) {
    const auto& taa = cfg.taa;
    TaaPC pc{};
    std::memcpy(pc.reproj, taa.reproj_matrix, sizeof(pc.reproj));
    pc.feedback_min = taa.feedback_min;
    pc.feedback_max = taa.feedback_max;
    pc.jitter_x     = taa.jitter_x;
    pc.jitter_y     = taa.jitter_y;
    pc.texel_x      = 1.0f / static_cast<float>(w ? w : 1);
    pc.texel_y      = 1.0f / static_cast<float>(h ? h : 1);
    // 行列未確定 / history 無効 (初回・カット・resize 後) は素通し。
    pc.valid = (taa.enabled && taa.matrix_valid && taa.history_valid) ? 1u : 0u;
    return pc;
}

SsrPC make_ssr_pc(const PostProcessConfig& cfg, uint32_t w, uint32_t h) {
    const auto& ssr = cfg.ssr;
    SsrPC pc{};
    pc.proj_xx    = ssr.proj_xx;
    pc.proj_yy    = ssr.proj_yy;
    pc.near_plane = ssr.near_plane;
    pc.far_plane  = ssr.far_plane;
    // disabled → intensity 0 で恒等へ縮退。
    pc.intensity  = ssr.enabled ? ssr.intensity : 0.0f;
    pc.stride_px  = ssr.stride_px;
    pc.thickness  = ssr.thickness;
    pc.max_steps  = std::min(ssr.max_steps, 64u);
    pc.texel_x    = 1.0f / static_cast<float>(w ? w : 1);
    pc.texel_y    = 1.0f / static_cast<float>(h ? h : 1);
    return pc;
}

ExposureMeasurePC make_exposure_measure_pc(const PostProcessConfig& cfg) {
    const auto& hdr = cfg.hdr;
    ExposureMeasurePC pc{};
    pc.min_lum = hdr.min_luminance;
    pc.max_lum = hdr.max_luminance;
    // 時間適応: blend = 1 - exp(-dt * rate)。 disabled は即時追従 (1.0)。
    const float dt = std::max(hdr.delta_seconds, 0.0f);
    pc.blend = hdr.auto_exposure
        ? std::min(1.0f, 1.0f - std::exp(-dt * hdr.adaptation_rate))
        : 1.0f;
    return pc;
}

ExposureApplyPC make_exposure_apply_pc(const PostProcessConfig& cfg) {
    ExposureApplyPC pc{};
    // disabled → key 0 で素通し (シェーダ側の恒等縮退)。
    pc.key = cfg.hdr.auto_exposure ? cfg.hdr.key : 0.0f;
    return pc;
}

MotionBlurVelocityPC make_motion_blur_velocity_pc(const PostProcessConfig& cfg) {
    const auto& mb = cfg.motion_blur;
    MotionBlurVelocityPC pc{};
    pc.intensity    = mb.intensity;
    pc.max_velocity = mb.max_velocity;
    pc.sample_count = mb.sample_count;
    pc.valid        = mb.enabled ? 1u : 0u;   // 行列不要 — enabled のみ
    return pc;
}

TaaVelocityPC make_taa_velocity_pc(const PostProcessConfig& cfg,
                                   uint32_t w, uint32_t h) {
    const auto& taa = cfg.taa;
    TaaVelocityPC pc{};
    pc.feedback_min = taa.feedback_min;
    pc.feedback_max = taa.feedback_max;
    pc.jitter_x     = taa.jitter_x;
    pc.jitter_y     = taa.jitter_y;
    pc.texel_x      = 1.0f / static_cast<float>(w ? w : 1);
    pc.texel_y      = 1.0f / static_cast<float>(h ? h : 1);
    // velocity 方式は行列不要 — history だけで駆動。
    pc.valid = (taa.enabled && taa.history_valid) ? 1u : 0u;
    return pc;
}

LensFlarePC make_lens_flare_pc(const PostProcessConfig& cfg,
                               uint32_t w, uint32_t h) {
    const auto& lf = cfg.lens_flare;
    LensFlarePC pc{};
    pc.intensity      = lf.enabled ? lf.intensity : 0.0f;
    pc.ghost_spacing  = lf.ghost_spacing;
    pc.halo_radius    = lf.halo_radius;
    pc.halo_intensity = lf.halo_intensity;
    pc.ghost_count    = std::min(lf.ghost_count, 8u);
    pc.texel_x        = 1.0f / static_cast<float>(w ? w : 1);
    pc.texel_y        = 1.0f / static_cast<float>(h ? h : 1);
    return pc;
}

FogPC make_fog_pc(const PostProcessConfig& cfg) {
    const auto& fog = cfg.volumetric_fog;
    FogPC pc{};
    std::memcpy(pc.cam_pos,   fog.camera_pos,     sizeof(pc.cam_pos));
    std::memcpy(pc.cam_fwd,   fog.camera_forward, sizeof(pc.cam_fwd));
    std::memcpy(pc.cam_right, fog.camera_right,   sizeof(pc.cam_right));
    std::memcpy(pc.cam_up,    fog.camera_up,      sizeof(pc.cam_up));
    std::memcpy(pc.sun_dir,   fog.sun_dir,        sizeof(pc.sun_dir));
    std::memcpy(pc.sun_color, fog.sun_color,      sizeof(pc.sun_color));
    std::memcpy(pc.fog_color, fog.color,          sizeof(pc.fog_color));
    // disabled → density 0 で恒等縮退。 カメラ未設定フレームは valid 0。
    pc.density        = fog.enabled ? fog.density : 0.0f;
    pc.height_falloff = fog.height_falloff;
    pc.base_height    = fog.base_height;
    pc.start_dist     = fog.start_distance;
    pc.phase_g        = fog.phase_g;
    pc.sun_scatter    = fog.sun_scatter;
    pc.near_plane     = fog.near_plane;
    pc.far_plane      = fog.far_plane;
    pc.tan_x          = fog.tan_half_fov_x;
    pc.tan_y          = fog.tan_half_fov_y;
    pc.valid          = (fog.enabled && fog.camera_valid) ? 1u : 0u;
    return pc;
}

SsgiGatherPC make_ssgi_gather_pc(const PostProcessConfig& cfg,
                                 uint32_t w, uint32_t h) {
    const auto& gi = cfg.ssgi;
    SsgiGatherPC pc{};
    pc.proj_xx      = gi.proj_xx;
    pc.proj_yy      = gi.proj_yy;
    pc.near_plane   = gi.near_plane;
    pc.far_plane    = gi.far_plane;
    pc.radius_px    = gi.radius_px;
    pc.feedback     = gi.feedback;
    // gather は half-res で走る — texel はフル解像度基準で渡し、 シェーダが
    // UV 空間で扱う (UV は解像度非依存)。
    pc.texel_x      = 1.0f / static_cast<float>(w ? w : 1);
    pc.texel_y      = 1.0f / static_cast<float>(h ? h : 1);
    pc.sample_count = std::min(gi.sample_count, 16u);
    return pc;
}

SsgiApplyPC make_ssgi_apply_pc(const PostProcessConfig& cfg) {
    SsgiApplyPC pc{};
    pc.intensity = cfg.ssgi.enabled ? cfg.ssgi.intensity : 0.0f;
    return pc;
}

GradePC make_grade_pc(const PostProcessConfig& cfg, bool output_is_srgb,
                      bool lut_loaded) {
    const auto& bloom = cfg.bloom;
    const auto& tm    = cfg.tone_mapping;
    const auto& vig   = cfg.vignette;
    const auto& cg    = cfg.color_grading;

    GradePC pc{};
    pc.bloom_intensity = bloom.enabled ? bloom.intensity : 0.0f;
    pc.tonemap_op      = tm.enabled ? static_cast<uint32_t>(tm.op) : 4u;
    pc.exposure        = tm.enabled ? tm.exposure : 1.0f;
    // sRGB swapchain は linear→sRGB をハードウェアで行う → 二重ガンマ回避。
    pc.gamma           = output_is_srgb ? 1.0f : tm.gamma;
    pc.white_point     = tm.white_point;
    pc.saturation      = tm.enabled ? tm.saturation : 1.0f;
    pc.vignette_intensity = vig.enabled ? vig.intensity : 0.0f;
    pc.vignette_radius    = vig.radius;
    pc.vignette_softness  = vig.softness;
    pc.lut_intensity      = (cg.enabled && lut_loaded) ? cg.lut_intensity : 0.0f;
    pc.lut_size           = static_cast<float>(cg.lut_size);
    pc.vig_r = vig.color[0];
    pc.vig_g = vig.color[1];
    pc.vig_b = vig.color[2];

    const auto& ca    = cfg.chromatic_aberration;
    const auto& grain = cfg.film_grain;
    pc.ca_intensity    = ca.enabled ? ca.intensity : 0.0f;
    pc.ca_start_radius = ca.start_radius;
    pc.grain_intensity = grain.enabled ? grain.intensity : 0.0f;
    pc.grain_response  = grain.response;
    pc.grain_seed      = grain.seed;
    return pc;
}

// 16-byte push field レイアウト記述 (デバッグ / 検証用)。
std::vector<PushFieldDesc> extract_layout() {
    return {
        {"threshold",      PushFieldType::FLOAT, 0},
        {"soft_threshold", PushFieldType::FLOAT, 4},
        {"_pad0",          PushFieldType::FLOAT, 8},
        {"_pad1",          PushFieldType::FLOAT, 12},
    };
}
std::vector<PushFieldDesc> blur_layout() {
    return {
        {"dir_x",  PushFieldType::FLOAT, 0},
        {"dir_y",  PushFieldType::FLOAT, 4},
        {"radius", PushFieldType::FLOAT, 8},
        {"_pad0",  PushFieldType::FLOAT, 12},
    };
}
std::vector<PushFieldDesc> bloom_down_layout() {
    return {
        {"texel_x", PushFieldType::FLOAT, 0},
        {"texel_y", PushFieldType::FLOAT, 4},
        {"_pad0",   PushFieldType::FLOAT, 8},
        {"_pad1",   PushFieldType::FLOAT, 12},
    };
}
std::vector<PushFieldDesc> bloom_up_layout() {
    return {
        {"texel_x", PushFieldType::FLOAT, 0},
        {"texel_y", PushFieldType::FLOAT, 4},
        {"scatter", PushFieldType::FLOAT, 8},
        {"_pad0",   PushFieldType::FLOAT, 12},
    };
}
std::vector<PushFieldDesc> dof_layout() {
    return {
        {"focus_distance", PushFieldType::FLOAT, 0},
        {"focus_range",    PushFieldType::FLOAT, 4},
        {"bokeh_radius",   PushFieldType::FLOAT, 8},
        {"near_start",     PushFieldType::FLOAT, 12},
        {"near_end",       PushFieldType::FLOAT, 16},
        {"far_start",      PushFieldType::FLOAT, 20},
        {"far_end",        PushFieldType::FLOAT, 24},
        {"sample_count",   PushFieldType::UINT,  28},
        {"texel_x",        PushFieldType::FLOAT, 32},
        {"texel_y",        PushFieldType::FLOAT, 36},
        {"near_plane",     PushFieldType::FLOAT, 40},
        {"far_plane",      PushFieldType::FLOAT, 44},
    };
}
std::vector<PushFieldDesc> ssao_layout() {
    return {
        {"radius_px",    PushFieldType::FLOAT, 0},
        {"bias",         PushFieldType::FLOAT, 4},
        {"range",        PushFieldType::FLOAT, 8},
        {"intensity",    PushFieldType::FLOAT, 12},
        {"power",        PushFieldType::FLOAT, 16},
        {"texel_x",      PushFieldType::FLOAT, 20},
        {"texel_y",      PushFieldType::FLOAT, 24},
        {"sample_count", PushFieldType::UINT,  28},
    };
}
std::vector<PushFieldDesc> motion_blur_layout() {
    std::vector<PushFieldDesc> fields;
    fields.reserve(20);
    for (uint32_t i = 0; i < 16; ++i) {
        fields.push_back({"reproj[" + std::to_string(i) + "]",
                          PushFieldType::FLOAT, i * 4});
    }
    fields.push_back({"intensity",    PushFieldType::FLOAT, 64});
    fields.push_back({"max_velocity", PushFieldType::FLOAT, 68});
    fields.push_back({"sample_count", PushFieldType::UINT,  72});
    fields.push_back({"valid",        PushFieldType::UINT,  76});
    return fields;
}
std::vector<PushFieldDesc> fxaa_layout() {
    return {
        {"texel_x",            PushFieldType::FLOAT, 0},
        {"texel_y",            PushFieldType::FLOAT, 4},
        {"edge_threshold",     PushFieldType::FLOAT, 8},
        {"edge_threshold_min", PushFieldType::FLOAT, 12},
        {"subpix_quality",     PushFieldType::FLOAT, 16},
        {"_pad0",              PushFieldType::FLOAT, 20},
        {"_pad1",              PushFieldType::FLOAT, 24},
        {"_pad2",              PushFieldType::FLOAT, 28},
    };
}
std::vector<PushFieldDesc> taa_layout() {
    std::vector<PushFieldDesc> fields;
    fields.reserve(24);
    for (uint32_t i = 0; i < 16; ++i) {
        fields.push_back({"reproj[" + std::to_string(i) + "]",
                          PushFieldType::FLOAT, i * 4});
    }
    fields.push_back({"feedback_min", PushFieldType::FLOAT, 64});
    fields.push_back({"feedback_max", PushFieldType::FLOAT, 68});
    fields.push_back({"jitter_x",     PushFieldType::FLOAT, 72});
    fields.push_back({"jitter_y",     PushFieldType::FLOAT, 76});
    fields.push_back({"texel_x",      PushFieldType::FLOAT, 80});
    fields.push_back({"texel_y",      PushFieldType::FLOAT, 84});
    fields.push_back({"valid",        PushFieldType::UINT,  88});
    fields.push_back({"_pad0",        PushFieldType::UINT,  92});
    return fields;
}
std::vector<PushFieldDesc> ssr_layout() {
    return {
        {"proj_xx",    PushFieldType::FLOAT, 0},
        {"proj_yy",    PushFieldType::FLOAT, 4},
        {"near_plane", PushFieldType::FLOAT, 8},
        {"far_plane",  PushFieldType::FLOAT, 12},
        {"intensity",  PushFieldType::FLOAT, 16},
        {"stride_px",  PushFieldType::FLOAT, 20},
        {"thickness",  PushFieldType::FLOAT, 24},
        {"max_steps",  PushFieldType::UINT,  28},
        {"texel_x",    PushFieldType::FLOAT, 32},
        {"texel_y",    PushFieldType::FLOAT, 36},
        {"_pad0",      PushFieldType::FLOAT, 40},
        {"_pad1",      PushFieldType::FLOAT, 44},
    };
}
std::vector<PushFieldDesc> exposure_measure_layout() {
    return {
        {"min_lum", PushFieldType::FLOAT, 0},
        {"max_lum", PushFieldType::FLOAT, 4},
        {"blend",   PushFieldType::FLOAT, 8},
        {"_pad0",   PushFieldType::FLOAT, 12},
    };
}
std::vector<PushFieldDesc> exposure_apply_layout() {
    return {
        {"key",   PushFieldType::FLOAT, 0},
        {"_pad0", PushFieldType::FLOAT, 4},
        {"_pad1", PushFieldType::FLOAT, 8},
        {"_pad2", PushFieldType::FLOAT, 12},
    };
}
std::vector<PushFieldDesc> motion_blur_velocity_layout() {
    return {
        {"intensity",    PushFieldType::FLOAT, 0},
        {"max_velocity", PushFieldType::FLOAT, 4},
        {"sample_count", PushFieldType::UINT,  8},
        {"valid",        PushFieldType::UINT,  12},
    };
}
std::vector<PushFieldDesc> taa_velocity_layout() {
    return {
        {"feedback_min", PushFieldType::FLOAT, 0},
        {"feedback_max", PushFieldType::FLOAT, 4},
        {"jitter_x",     PushFieldType::FLOAT, 8},
        {"jitter_y",     PushFieldType::FLOAT, 12},
        {"texel_x",      PushFieldType::FLOAT, 16},
        {"texel_y",      PushFieldType::FLOAT, 20},
        {"valid",        PushFieldType::UINT,  24},
        {"_pad0",        PushFieldType::UINT,  28},
    };
}
std::vector<PushFieldDesc> lens_flare_layout() {
    return {
        {"intensity",      PushFieldType::FLOAT, 0},
        {"ghost_spacing",  PushFieldType::FLOAT, 4},
        {"halo_radius",    PushFieldType::FLOAT, 8},
        {"halo_intensity", PushFieldType::FLOAT, 12},
        {"ghost_count",    PushFieldType::UINT,  16},
        {"texel_x",        PushFieldType::FLOAT, 20},
        {"texel_y",        PushFieldType::FLOAT, 24},
        {"_pad0",          PushFieldType::FLOAT, 28},
    };
}
std::vector<PushFieldDesc> fog_layout() {
    // vec3 + スカラーの 16B 行 × 8 (128B)。 名前は行単位でまとめる。
    std::vector<PushFieldDesc> fields;
    const char* rows[8] = {"cam_pos+density",    "cam_fwd+height_falloff",
                           "cam_right+base_h",   "cam_up+start_dist",
                           "sun_dir+phase_g",    "sun_color+sun_scatter",
                           "fog_color+near",     "far+tan_x+tan_y+valid"};
    for (uint32_t r = 0; r < 8; ++r) {
        for (uint32_t c = 0; c < 4; ++c) {
            const bool is_valid_slot = (r == 7 && c == 3);
            fields.push_back({std::string(rows[r]) + "[" + std::to_string(c) + "]",
                              is_valid_slot ? PushFieldType::UINT
                                            : PushFieldType::FLOAT,
                              r * 16 + c * 4});
        }
    }
    return fields;
}
std::vector<PushFieldDesc> ssgi_gather_layout() {
    return {
        {"proj_xx",      PushFieldType::FLOAT, 0},
        {"proj_yy",      PushFieldType::FLOAT, 4},
        {"near_plane",   PushFieldType::FLOAT, 8},
        {"far_plane",    PushFieldType::FLOAT, 12},
        {"radius_px",    PushFieldType::FLOAT, 16},
        {"feedback",     PushFieldType::FLOAT, 20},
        {"texel_x",      PushFieldType::FLOAT, 24},
        {"texel_y",      PushFieldType::FLOAT, 28},
        {"sample_count", PushFieldType::UINT,  32},
        {"_pad0",        PushFieldType::FLOAT, 36},
        {"_pad1",        PushFieldType::FLOAT, 40},
        {"_pad2",        PushFieldType::FLOAT, 44},
    };
}
std::vector<PushFieldDesc> ssgi_apply_layout() {
    return {
        {"intensity", PushFieldType::FLOAT, 0},
        {"_pad0",     PushFieldType::FLOAT, 4},
        {"_pad1",     PushFieldType::FLOAT, 8},
        {"_pad2",     PushFieldType::FLOAT, 12},
    };
}
std::vector<PushFieldDesc> grade_layout() {
    return {
        {"bloom_intensity",    PushFieldType::FLOAT, 0},
        {"tonemap_op",         PushFieldType::UINT,  4},
        {"exposure",           PushFieldType::FLOAT, 8},
        {"gamma",              PushFieldType::FLOAT, 12},
        {"white_point",        PushFieldType::FLOAT, 16},
        {"saturation",         PushFieldType::FLOAT, 20},
        {"vignette_intensity", PushFieldType::FLOAT, 24},
        {"vignette_radius",    PushFieldType::FLOAT, 28},
        {"vignette_softness",  PushFieldType::FLOAT, 32},
        {"lut_intensity",      PushFieldType::FLOAT, 36},
        {"lut_size",           PushFieldType::FLOAT, 40},
        {"vig_r",              PushFieldType::FLOAT, 44},
        {"vig_g",              PushFieldType::FLOAT, 48},
        {"vig_b",              PushFieldType::FLOAT, 52},
        {"ca_intensity",       PushFieldType::FLOAT, 56},
        {"ca_start_radius",    PushFieldType::FLOAT, 60},
        {"grain_intensity",    PushFieldType::FLOAT, 64},
        {"grain_response",     PushFieldType::FLOAT, 68},
        {"grain_seed",         PushFieldType::FLOAT, 72},
    };
}

} // namespace

PostProcessChain build_post_process_chain(const PostProcessConfig& cfg,
                                          const std::string&       shader_dir,
                                          uint32_t                 extent_w,
                                          uint32_t                 extent_h,
                                          bool                     output_is_srgb,
                                          bool                     lut_loaded,
                                          const std::vector<PostProcessPassDef>& extra) {
    PostProcessChain chain;

    const std::string fs_vert = shader_dir + "/fullscreen_quad.vert.spv";

    // ── 組み込み pass テンプレート: [dof →] extract → blur H → blur V → grade ──
    //    旧 PostProcessPipeline::record() の 4-pass と同一構造。 disabled な
    //    エフェクトは push constant で恒等へ縮退するため、 4 pass は常に
    //    実行され中間 image レイアウトを毎フレーム有効に保つ。
    //    DoF は有効時のみ pass を挿入する構造変更 (深度をパイプラインへ
    //    捩じ込む必要があるため常設にしない) — 無効時は旧 4-pass と同一。

    // 後段が「シーンカラー」 として読む論理名。 DoF 有効時は DoF 出力へ差替わる。
    const char* scene_src = kPostProcessSceneTarget;

    // Pass 0 (optional): depth of field  scene + depth → pp_dof
    if (cfg.depth_of_field.enabled) {
        PostProcessPassDef p;
        p.name        = "dof";
        p.vert_spv    = fs_vert;
        p.frag_spv    = shader_dir + "/dof.frag.spv";
        p.inputs      = {kPostProcessSceneTarget, kPostProcessDepthTarget};
        p.output      = kDofTarget;
        p.push_layout = dof_layout();
        store_pc(p.push_data, make_dof_pc(cfg, extent_w, extent_h));
        chain.passes.push_back(std::move(p));
        scene_src = kDofTarget;
    }

    // Pass 0b (optional): SSAO (PP 近似)  scene + depth → pp_ssao
    //   深度差から遮蔽を推定してシーン色へ乗算する。 bloom より前に置き、
    //   AO の掛かった色が bloom 抽出 / 合成の入力になるようにする。
    if (cfg.ssao.enabled) {
        PostProcessPassDef p;
        p.name        = "ssao_apply";
        p.vert_spv    = fs_vert;
        p.frag_spv    = shader_dir + "/ssao_apply.frag.spv";
        p.inputs      = {scene_src, kPostProcessDepthTarget};
        p.output      = kSsaoTarget;
        p.push_layout = ssao_layout();
        store_pc(p.push_data, make_ssao_pc(cfg, extent_w, extent_h));
        chain.passes.push_back(std::move(p));
        scene_src = kSsaoTarget;
    }

    // Pass 0b2 (optional): SSGI  gather (half-res + 時間フィルタ) → apply
    //   画面内バウンスを加算する。 AO 適用後・SSR 前 (反射は GI 込みの色を拾う)。
    if (cfg.ssgi.enabled) {
        {
            PostProcessPassDef p;
            p.name        = "ssgi_gather";
            p.vert_spv    = fs_vert;
            p.frag_spv    = shader_dir + "/ssgi_gather.frag.spv";
            p.inputs      = {scene_src, kPostProcessDepthTarget,
                             history_input_name(kSsgiTarget)};
            p.output      = kSsgiTarget;
            p.push_layout = ssgi_gather_layout();
            store_pc(p.push_data, make_ssgi_gather_pc(cfg, extent_w, extent_h));
            chain.passes.push_back(std::move(p));
            chain.target_divisors.push_back({kSsgiTarget, 2u});   // half-res
        }
        {
            PostProcessPassDef p;
            p.name        = "ssgi_apply";
            p.vert_spv    = fs_vert;
            p.frag_spv    = shader_dir + "/ssgi_apply.frag.spv";
            p.inputs      = {scene_src, kSsgiTarget};
            p.output      = kSsgiOutTarget;
            p.push_layout = ssgi_apply_layout();
            store_pc(p.push_data, make_ssgi_apply_pc(cfg));
            chain.passes.push_back(std::move(p));
            scene_src = kSsgiOutTarget;
        }
    }

    // Pass 0c (optional): SSR  scene + depth → pp_ssr
    //   深度から view 空間を再構築 (法線は深度勾配) してスクリーンスペースの
    //   レイマーチで反射を合成する。 AO 適用後・motion blur 前。
    if (cfg.ssr.enabled) {
        PostProcessPassDef p;
        p.name        = "ssr";
        p.vert_spv    = fs_vert;
        p.frag_spv    = shader_dir + "/ssr.frag.spv";
        p.inputs      = {scene_src, kPostProcessDepthTarget};
        p.output      = kSsrTarget;
        p.push_layout = ssr_layout();
        store_pc(p.push_data, make_ssr_pc(cfg, extent_w, extent_h));
        chain.passes.push_back(std::move(p));
        scene_src = kSsrTarget;
    }

    // Pass 0c2 (optional): volumetric fog  scene + depth → pp_fog
    //   解析的 height fog + 太陽前方散乱。 反射/GI の後、 motion blur 前。
    if (cfg.volumetric_fog.enabled) {
        PostProcessPassDef p;
        p.name        = "volumetric_fog";
        p.vert_spv    = fs_vert;
        p.frag_spv    = shader_dir + "/volumetric_fog.frag.spv";
        p.inputs      = {scene_src, kPostProcessDepthTarget};
        p.output      = kFogTarget;
        p.push_layout = fog_layout();
        store_pc(p.push_data, make_fog_pc(cfg));
        chain.passes.push_back(std::move(p));
        scene_src = kFogTarget;
    }

    // velocity buffer 方式が使えるか (MRT 有効時のみ)。
    const bool velocity_ok = cfg.velocity.enabled;

    // Pass 0d (optional): motion blur  → pp_mblur (HDR)
    //   velocity buffer 方式 (per_object、 要 MRT) はスクリーン速度を直接読む。
    //   それ以外は深度 + カメラ再投影 (prevVP * inv currVP) 方式。
    if (cfg.motion_blur.enabled) {
        const bool per_object = cfg.motion_blur.per_object && velocity_ok;
        PostProcessPassDef p;
        p.name     = "motion_blur";
        p.vert_spv = fs_vert;
        if (per_object) {
            p.frag_spv    = shader_dir + "/motion_blur_velocity.frag.spv";
            p.inputs      = {scene_src, kPostProcessVelocityTarget};
            p.push_layout = motion_blur_velocity_layout();
            store_pc(p.push_data, make_motion_blur_velocity_pc(cfg));
        } else {
            p.frag_spv    = shader_dir + "/motion_blur.frag.spv";
            p.inputs      = {scene_src, kPostProcessDepthTarget};
            p.push_layout = motion_blur_layout();
            store_pc(p.push_data, make_motion_blur_pc(cfg));
        }
        p.output = kMBlurTarget;
        chain.passes.push_back(std::move(p));
        scene_src = kMBlurTarget;
    }

    // Pass 0e (optional): TAA  → pp_taa (HDR)
    //   history buffer (`__history:pp_taa__`) は前フレームの pp_taa。
    //   pipeline がフレーム末尾に pp_taa → history のコピーを記録する。
    //   velocity buffer 方式 (use_velocity、 要 MRT) は動体を正確に追跡する。
    if (cfg.taa.enabled) {
        const bool use_velocity = cfg.taa.use_velocity && velocity_ok;
        PostProcessPassDef p;
        p.name     = "taa";
        p.vert_spv = fs_vert;
        if (use_velocity) {
            p.frag_spv    = shader_dir + "/taa_velocity.frag.spv";
            p.inputs      = {scene_src, kPostProcessDepthTarget,
                             history_input_name(kTaaTarget),
                             kPostProcessVelocityTarget};
            p.push_layout = taa_velocity_layout();
            store_pc(p.push_data, make_taa_velocity_pc(cfg, extent_w, extent_h));
        } else {
            p.frag_spv    = shader_dir + "/taa.frag.spv";
            p.inputs      = {scene_src, kPostProcessDepthTarget,
                             history_input_name(kTaaTarget)};
            p.push_layout = taa_layout();
            store_pc(p.push_data, make_taa_pc(cfg, extent_w, extent_h));
        }
        p.output = kTaaTarget;
        chain.passes.push_back(std::move(p));
        scene_src = kTaaTarget;
    }

    // Pass 0f (optional): auto exposure — 計測 (1x1 viewport) + 適用の 2 pass。
    //   計測は前フレームの適応値 (`__history:pp_exposure__`) と混合して
    //   時間適応する。 適用は bloom / grade より前 (露出済みの値で抽出する)。
    if (cfg.hdr.auto_exposure) {
        {
            PostProcessPassDef p;
            p.name        = "exposure_measure";
            p.vert_spv    = fs_vert;
            p.frag_spv    = shader_dir + "/exposure_measure.frag.spv";
            p.inputs      = {scene_src, history_input_name(kExposureTarget)};
            p.output      = kExposureTarget;
            p.push_layout = exposure_measure_layout();
            p.viewport_w  = 1;
            p.viewport_h  = 1;
            store_pc(p.push_data, make_exposure_measure_pc(cfg));
            chain.passes.push_back(std::move(p));
        }
        {
            PostProcessPassDef p;
            p.name        = "exposure_apply";
            p.vert_spv    = fs_vert;
            p.frag_spv    = shader_dir + "/exposure_apply.frag.spv";
            p.inputs      = {scene_src, kExposureTarget};
            p.output      = kExposedTarget;
            p.push_layout = exposure_apply_layout();
            store_pc(p.push_data, make_exposure_apply_pc(cfg));
            chain.passes.push_back(std::move(p));
            scene_src = kExposedTarget;
        }
    }

    // grade が bloom として読む論理名 (方式で異なる)。
    std::string bloom_src = kPingTarget;

    if (cfg.bloom.mip_chain) {
        // ── mip チェーン方式: extract(1/2) → down… → up… (縮小ターゲット) ──
        //    広く柔らかい bloom。 ターゲットは target_divisors で縮小宣言する。
        const uint32_t mips = bloom_mip_count(cfg, extent_w, extent_h);
        auto down_name = [](uint32_t k) {
            return "pp_bloom_d" + std::to_string(k);
        };
        auto up_name = [](uint32_t k) {
            return "pp_bloom_u" + std::to_string(k);
        };

        // Pass 1: bright-pass extraction  scene → d0 (1/2)
        {
            PostProcessPassDef p;
            p.name        = "bloom_extract";
            p.vert_spv    = fs_vert;
            p.frag_spv    = shader_dir + "/bloom_extract.frag.spv";
            p.inputs      = {scene_src};
            p.output      = down_name(0);
            p.push_layout = extract_layout();
            store_pc(p.push_data, make_extract_pc(cfg));
            chain.passes.push_back(std::move(p));
            chain.target_divisors.push_back({down_name(0), 2u});
        }
        // Pass 2..M: downsample  d(k-1) → d(k)
        for (uint32_t k = 1; k < mips; ++k) {
            PostProcessPassDef p;
            p.name        = "bloom_down_" + std::to_string(k);
            p.vert_spv    = fs_vert;
            p.frag_spv    = shader_dir + "/bloom_down.frag.spv";
            p.inputs      = {down_name(k - 1)};
            p.output      = down_name(k);
            p.push_layout = bloom_down_layout();
            store_pc(p.push_data, make_bloom_down_pc(k, extent_w, extent_h));
            chain.passes.push_back(std::move(p));
            chain.target_divisors.push_back({down_name(k), 1u << (k + 1)});
        }
        // Pass M+1..: upsample  (下位 mip + skip 接続) → u(k)。 k = M-2 .. 0。
        for (uint32_t i = 0; i < mips - 1; ++i) {
            const uint32_t k = mips - 2 - i;
            const std::string lower =
                (k == mips - 2) ? down_name(mips - 1) : up_name(k + 1);
            PostProcessPassDef p;
            p.name        = "bloom_up_" + std::to_string(k);
            p.vert_spv    = fs_vert;
            p.frag_spv    = shader_dir + "/bloom_up.frag.spv";
            p.inputs      = {lower, down_name(k)};
            p.output      = up_name(k);
            p.push_layout = bloom_up_layout();
            store_pc(p.push_data, make_bloom_up_pc(cfg, k, extent_w, extent_h));
            chain.passes.push_back(std::move(p));
            chain.target_divisors.push_back({up_name(k), 1u << (k + 1)});
        }
        // grade は最上位 upsample (1/2 解像度) を bilinear で読む。
        bloom_src = up_name(0);
    } else {
        // ── 従来方式: extract → blur H → blur V (フルレス separable) ──
        // Pass 1: bright-pass extraction  scene → ping
        {
            PostProcessPassDef p;
            p.name        = "bloom_extract";
            p.vert_spv    = fs_vert;
            p.frag_spv    = shader_dir + "/bloom_extract.frag.spv";
            p.inputs      = {scene_src};
            p.output      = kPingTarget;
            p.push_layout = extract_layout();
            store_pc(p.push_data, make_extract_pc(cfg));
            chain.passes.push_back(std::move(p));
        }
        // Pass 2: horizontal blur  ping → pong
        {
            PostProcessPassDef p;
            p.name        = "bloom_blur_h";
            p.vert_spv    = fs_vert;
            p.frag_spv    = shader_dir + "/bloom_blur.frag.spv";
            p.inputs      = {kPingTarget};
            p.output      = kPongTarget;
            p.push_layout = blur_layout();
            store_pc(p.push_data, make_blur_h_pc(cfg, extent_w));
            chain.passes.push_back(std::move(p));
        }
        // Pass 3: vertical blur  pong → ping
        {
            PostProcessPassDef p;
            p.name        = "bloom_blur_v";
            p.vert_spv    = fs_vert;
            p.frag_spv    = shader_dir + "/bloom_blur.frag.spv";
            p.inputs      = {kPongTarget};
            p.output      = kPingTarget;
            p.push_layout = blur_layout();
            store_pc(p.push_data, make_blur_v_pc(cfg, extent_h));
            chain.passes.push_back(std::move(p));
        }
    }
    // Pass 4b (optional): lens flare — bloom 抽出結果へ ghost + halo を
    //   加算した pp_flare を作り、 grade の bloom 入力を差し替える
    //   (grade 自体は無変更 — bloom 経路への割り込み)。
    if (cfg.lens_flare.enabled) {
        PostProcessPassDef p;
        p.name        = "lens_flare";
        p.vert_spv    = fs_vert;
        p.frag_spv    = shader_dir + "/lens_flare.frag.spv";
        p.inputs      = {bloom_src};
        p.output      = kFlareTarget;
        p.push_layout = lens_flare_layout();
        store_pc(p.push_data, make_lens_flare_pc(cfg, extent_w, extent_h));
        chain.passes.push_back(std::move(p));
        // flare 出力は bloom と同解像度 (mip チェーン時は 1/2)。
        const uint32_t bloom_div = post_process_target_divisor(chain, bloom_src);
        if (bloom_div > 1) {
            chain.target_divisors.push_back({kFlareTarget, bloom_div});
        }
        bloom_src = kFlareTarget;
    }

    // TAA 有効時は FXAA を外す (二重 AA 防止 — TAA 優先、 spec §3.2)。
    const bool fxaa_on = cfg.fxaa.enabled && !cfg.taa.enabled;

    // Pass 4: final composite (bloom + tonemap + LUT + vignette + CA + grain)
    //   scene+ping+lut → output (FXAA 有効時は pp_ldr へ差し替え)
    {
        PostProcessPassDef p;
        p.name        = "color_grade";
        p.vert_spv    = fs_vert;
        p.frag_spv    = shader_dir + "/color_grade.frag.spv";
        p.inputs      = {scene_src, bloom_src, kPostProcessLutTarget};
        p.output      = fxaa_on ? kLdrTarget
                                : kPostProcessOutputTarget;
        p.push_layout = grade_layout();
        store_pc(p.push_data, make_grade_pc(cfg, output_is_srgb, lut_loaded));
        chain.passes.push_back(std::move(p));
    }

    // Pass 5 (optional): FXAA  pp_ldr → output
    //   トーンマップ後の LDR で走る。 grade が輝度を alpha へ書くため
    //   シェーダ側の luma 計算は alpha 読みで済む。
    if (fxaa_on) {
        PostProcessPassDef p;
        p.name        = "fxaa";
        p.vert_spv    = fs_vert;
        p.frag_spv    = shader_dir + "/fxaa.frag.spv";
        p.inputs      = {kLdrTarget};
        p.output      = kPostProcessOutputTarget;
        p.push_layout = fxaa_layout();
        store_pc(p.push_data, make_fxaa_pc(cfg, extent_w, extent_h));
        chain.passes.push_back(std::move(p));
    }

    // ── 任意追加 pass (ホスト定義)。 現状 KS は空で呼ぶ ──────────────────
    for (const auto& e : extra) chain.passes.push_back(e);

    // 中間ターゲット (予約名を除く) を pass の入出力から収集する。
    rebuild_intermediate_targets(chain);

    return chain;
}

void refresh_post_process_chain(PostProcessChain&        chain,
                                const PostProcessConfig& cfg,
                                uint32_t                 extent_w,
                                uint32_t                 extent_h,
                                bool                     output_is_srgb,
                                bool                     lut_loaded) {
    // 組み込み pass の push_data だけを config / 解像度に合わせて詰め直す。
    // pass 名で識別する (任意 extra pass は触らない)。 DoF の有効 / 無効
    // 切替は pass の増減 = 構造変更なのでここでは扱わない
    // (`PostProcessPipeline::rebuild_chain()` の領分)。
    for (auto& p : chain.passes) {
        if (p.name == "bloom_extract") {
            store_pc(p.push_data, make_extract_pc(cfg));
        } else if (p.name == "bloom_blur_h") {
            store_pc(p.push_data, make_blur_h_pc(cfg, extent_w));
        } else if (p.name == "bloom_blur_v") {
            store_pc(p.push_data, make_blur_v_pc(cfg, extent_h));
        } else if (p.name == "color_grade") {
            store_pc(p.push_data, make_grade_pc(cfg, output_is_srgb, lut_loaded));
        } else if (p.name == "dof") {
            store_pc(p.push_data, make_dof_pc(cfg, extent_w, extent_h));
        } else if (p.name == "ssao_apply") {
            store_pc(p.push_data, make_ssao_pc(cfg, extent_w, extent_h));
        } else if (p.name == "motion_blur") {
            // velocity 方式かは構築時のシェーダで決まる — push サイズで判別。
            if (p.push_data.size() == sizeof(MotionBlurVelocityPC)) {
                store_pc(p.push_data, make_motion_blur_velocity_pc(cfg));
            } else {
                store_pc(p.push_data, make_motion_blur_pc(cfg));
            }
        } else if (p.name == "fxaa") {
            store_pc(p.push_data, make_fxaa_pc(cfg, extent_w, extent_h));
        } else if (p.name == "ssr") {
            store_pc(p.push_data, make_ssr_pc(cfg, extent_w, extent_h));
        } else if (p.name == "taa") {
            if (p.push_data.size() == sizeof(TaaVelocityPC)) {
                store_pc(p.push_data,
                         make_taa_velocity_pc(cfg, extent_w, extent_h));
            } else {
                store_pc(p.push_data, make_taa_pc(cfg, extent_w, extent_h));
            }
        } else if (p.name == "volumetric_fog") {
            store_pc(p.push_data, make_fog_pc(cfg));
        } else if (p.name == "lens_flare") {
            store_pc(p.push_data, make_lens_flare_pc(cfg, extent_w, extent_h));
        } else if (p.name == "ssgi_gather") {
            store_pc(p.push_data, make_ssgi_gather_pc(cfg, extent_w, extent_h));
        } else if (p.name == "ssgi_apply") {
            store_pc(p.push_data, make_ssgi_apply_pc(cfg));
        } else if (p.name == "exposure_measure") {
            store_pc(p.push_data, make_exposure_measure_pc(cfg));
        } else if (p.name == "exposure_apply") {
            store_pc(p.push_data, make_exposure_apply_pc(cfg));
        } else if (p.name.rfind("bloom_down_", 0) == 0) {
            const uint32_t k = static_cast<uint32_t>(
                std::strtoul(p.name.c_str() + 11, nullptr, 10));
            store_pc(p.push_data, make_bloom_down_pc(k, extent_w, extent_h));
        } else if (p.name.rfind("bloom_up_", 0) == 0) {
            const uint32_t k = static_cast<uint32_t>(
                std::strtoul(p.name.c_str() + 9, nullptr, 10));
            store_pc(p.push_data,
                     make_bloom_up_pc(cfg, k, extent_w, extent_h));
        }
    }
}

// ============================================================
// チェーン途中編集 API
// ============================================================

uint32_t post_process_target_divisor(const PostProcessChain& chain,
                                     std::string_view name) {
    for (const auto& [n, div] : chain.target_divisors) {
        if (n == name) return div > 0 ? div : 1;
    }
    return 1;
}

int find_post_process_pass(const PostProcessChain& chain, std::string_view name) {
    for (size_t i = 0; i < chain.passes.size(); ++i) {
        if (chain.passes[i].name == name) return static_cast<int>(i);
    }
    return -1;
}

bool insert_post_process_pass(PostProcessChain&  chain,
                              PostProcessPassDef pass,
                              std::string_view   anchor,
                              PassInsertWhere    where) {
    const int at = find_post_process_pass(chain, anchor);
    if (at < 0) return false;                                   // anchor 不在
    if (find_post_process_pass(chain, pass.name) >= 0) return false;  // 重複名

    const size_t pos = static_cast<size_t>(at) +
                       (where == PassInsertWhere::AFTER ? 1u : 0u);
    chain.passes.insert(chain.passes.begin() + static_cast<ptrdiff_t>(pos),
                        std::move(pass));
    rebuild_intermediate_targets(chain);
    return true;
}

bool remove_post_process_pass(PostProcessChain& chain, std::string_view name) {
    const int at = find_post_process_pass(chain, name);
    if (at < 0) return false;
    chain.passes.erase(chain.passes.begin() + at);
    rebuild_intermediate_targets(chain);
    return true;
}

bool rebind_post_process_input(PostProcessChain& chain,
                               std::string_view  pass_name,
                               std::string_view  old_target,
                               std::string_view  new_target) {
    const int at = find_post_process_pass(chain, pass_name);
    if (at < 0) return false;
    bool rebound = false;
    for (auto& in : chain.passes[static_cast<size_t>(at)].inputs) {
        if (in == old_target) {
            in = std::string(new_target);
            rebound = true;
        }
    }
    if (rebound) rebuild_intermediate_targets(chain);
    return rebound;
}

void rebuild_intermediate_targets(PostProcessChain& chain) {
    auto is_reserved = [](const std::string& n) {
        return n == kPostProcessSceneTarget || n == kPostProcessOutputTarget ||
               n == kPostProcessLutTarget   || n == kPostProcessDepthTarget ||
               n == kPostProcessVelocityTarget ||
               // history 入力 (__history:<target>__) は persistent image に
               // 解決される — 中間ターゲットとして数えない。
               !parse_history_input(n).empty();
    };
    chain.intermediate_names.clear();
    auto add_unique = [&](const std::string& n) {
        if (n.empty() || is_reserved(n)) return;
        for (const auto& have : chain.intermediate_names)
            if (have == n) return;
        chain.intermediate_names.push_back(n);
    };
    for (const auto& p : chain.passes) {
        for (const auto& in : p.inputs) add_unique(in);
        add_unique(p.output);
    }
}

} // namespace pictor
