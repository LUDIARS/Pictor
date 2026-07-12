#include "pictor/postprocess/postprocess_chain.h"

#include "pictor/pipeline/pipeline_profile.h"

#include <cstring>

namespace pictor {

// 予約済み論理ターゲット名。
const char* const kPostProcessOutputTarget = "__output__";
const char* const kPostProcessSceneTarget  = "__scene__";
const char* const kPostProcessLutTarget    = "__lut__";
const char* const kPostProcessDepthTarget  = "__depth__";

namespace {

// 組み込みチェーンの中間ターゲット論理名 (ping-pong / DoF 出力)。
constexpr const char* kPingTarget = "pp_ping";
constexpr const char* kPongTarget = "pp_pong";
constexpr const char* kDofTarget  = "pp_dof";

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
};
// dof.frag の push constant (shaders/postprocess/dof.frag と同一レイアウト)。
struct DofPC {
    float    focus_distance, focus_range, bokeh_radius;
    float    near_start, near_end, far_start, far_end;
    uint32_t sample_count;
    float    texel_x, texel_y, pad0, pad1;
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
        {"_pad0",          PushFieldType::FLOAT, 40},
        {"_pad1",          PushFieldType::FLOAT, 44},
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
    // Pass 4: final composite (bloom + tonemap + LUT + vignette)  scene+ping+lut → output
    {
        PostProcessPassDef p;
        p.name        = "color_grade";
        p.vert_spv    = fs_vert;
        p.frag_spv    = shader_dir + "/color_grade.frag.spv";
        p.inputs      = {scene_src, kPingTarget, kPostProcessLutTarget};
        p.output      = kPostProcessOutputTarget;
        p.push_layout = grade_layout();
        store_pc(p.push_data, make_grade_pc(cfg, output_is_srgb, lut_loaded));
        chain.passes.push_back(std::move(p));
    }

    // ── 任意追加 pass (SSAO / FXAA 等)。 現状 KS は空で呼ぶ ──────────────
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
        }
    }
}

// ============================================================
// チェーン途中編集 API
// ============================================================

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
               n == kPostProcessLutTarget   || n == kPostProcessDepthTarget;
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
