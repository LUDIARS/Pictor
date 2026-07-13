/// PostProcessChain — 組み込みエフェクト → 汎用チェーン畳み込みの検証.
///
/// `spec/rendering-extensibility-design.md` §6.3 (phase 2 項目1)。
/// VkPipeline 生成は Vulkan device を要するためここでは検証しない。
/// チェーン構造 (pass 数 / 入出力ターゲット名 / push constant バイト列) と、
/// 既存 4 エフェクト (Bloom / ToneMapping / Vignette / ColorGrading) の
/// 「無効時は恒等へ縮退」 という挙動保存をバイト単位で見る。

#include "pictor/postprocess/postprocess_chain.h"
#include "test_common.h"

#include <cmath>
#include <cstring>

using namespace pictor;
using namespace pictor_test;

namespace {

// chain.cpp と同一レイアウトの push constant 構造体 (検証用ミラー)。
struct ExtractPC { float threshold, soft_threshold, pad0, pad1; };
struct BlurPC    { float dir_x, dir_y, radius, pad0; };
struct GradePC {
    float    bloom_intensity;
    uint32_t tonemap_op;
    float    exposure, gamma, white_point, saturation;
    float    vignette_intensity, vignette_radius, vignette_softness;
    float    lut_intensity, lut_size;
    float    vig_r, vig_g, vig_b;
    float    ca_intensity, ca_start_radius;
    float    grain_intensity, grain_response, grain_seed;
};
struct SsaoPC {
    float    radius_px, bias, range, intensity;
    float    power, texel_x, texel_y;
    uint32_t sample_count;
};
struct MotionBlurPC {
    float    reproj[16];
    float    intensity, max_velocity;
    uint32_t sample_count;
    uint32_t valid;
};
struct FxaaPC {
    float texel_x, texel_y;
    float edge_threshold, edge_threshold_min;
    float subpix_quality, pad0, pad1, pad2;
};
struct TaaPC {
    float    reproj[16];
    float    feedback_min, feedback_max;
    float    jitter_x, jitter_y;
    float    texel_x, texel_y;
    uint32_t valid;
    uint32_t pad0;
};
struct SsrPC {
    float    proj_xx, proj_yy, near_plane, far_plane;
    float    intensity, stride_px, thickness;
    uint32_t max_steps;
    float    texel_x, texel_y, pad0, pad1;
};
struct ExposureMeasurePC {
    float min_lum, max_lum, blend, pad0;
};
struct ExposureApplyPC {
    float key, pad0, pad1, pad2;
};

template <typename T>
T read_pc(const PostProcessPassDef& p) {
    T out{};
    if (p.push_data.size() >= sizeof(T))
        std::memcpy(&out, p.push_data.data(), sizeof(T));
    return out;
}

bool feq(float a, float b, float eps = 1e-4f) {
    return (a - b) < eps && (b - a) < eps;
}

const PostProcessPassDef* find_pass(const PostProcessChain& c, const char* name) {
    for (const auto& p : c.passes)
        if (p.name == name) return &p;
    return nullptr;
}

} // namespace

int main() {
    // 1. 組み込みチェーンは 4 pass (extract → blur H → blur V → grade)。
    {
        PostProcessConfig cfg;  // all defaults: bloom/tm/vignette on, cg off
        PostProcessChain chain = build_post_process_chain(
            cfg, "shaders", 1920, 1080, /*srgb=*/false, /*lut=*/false);

        PT_ASSERT_OP(chain.passes.size(), ==, size_t{4},
                     "built-in chain has exactly 4 passes");
        PT_ASSERT(chain.passes[0].name == "bloom_extract", "pass 0 = extract");
        PT_ASSERT(chain.passes[1].name == "bloom_blur_h",  "pass 1 = blur H");
        PT_ASSERT(chain.passes[2].name == "bloom_blur_v",  "pass 2 = blur V");
        PT_ASSERT(chain.passes[3].name == "color_grade",   "pass 3 = grade");
        PT_ASSERT_OP(chain.intermediate_names.size(), ==, size_t{2},
                     "two intermediate targets (ping/pong)");
    }

    // 2. 入出力ターゲットの配線が旧固定 4-pass と一致する。
    //    extract: scene→ping, blurH: ping→pong, blurV: pong→ping,
    //    grade: scene+ping+lut→output。
    {
        PostProcessConfig cfg;
        PostProcessChain chain = build_post_process_chain(
            cfg, "shaders", 1280, 720, false, false);

        const auto& ex = chain.passes[0];
        PT_ASSERT_OP(ex.inputs.size(), ==, size_t{1}, "extract: 1 input");
        PT_ASSERT(ex.inputs[0] == kPostProcessSceneTarget, "extract reads scene");
        PT_ASSERT(ex.output == "pp_ping", "extract writes ping");

        const auto& bh = chain.passes[1];
        PT_ASSERT(bh.inputs[0] == "pp_ping", "blur H reads ping");
        PT_ASSERT(bh.output == "pp_pong",    "blur H writes pong");

        const auto& bv = chain.passes[2];
        PT_ASSERT(bv.inputs[0] == "pp_pong", "blur V reads pong");
        PT_ASSERT(bv.output == "pp_ping",    "blur V writes ping");

        const auto& gr = chain.passes[3];
        PT_ASSERT_OP(gr.inputs.size(), ==, size_t{3}, "grade: 3 inputs");
        PT_ASSERT(gr.inputs[0] == kPostProcessSceneTarget, "grade input0 = scene");
        PT_ASSERT(gr.inputs[1] == "pp_ping",               "grade input1 = bloom");
        PT_ASSERT(gr.inputs[2] == kPostProcessLutTarget,   "grade input2 = lut");
        PT_ASSERT(gr.output == kPostProcessOutputTarget,   "grade writes swapchain");
    }

    // 3. 挙動保存 — 全エフェクト有効時の push constant が想定値どおり。
    {
        PostProcessConfig cfg;
        cfg.bloom.enabled        = true;
        cfg.bloom.threshold      = 0.75f;
        cfg.bloom.soft_threshold = 0.5f;
        cfg.bloom.intensity      = 0.55f;
        cfg.bloom.radius         = 3.0f;
        cfg.tone_mapping.enabled = true;
        cfg.tone_mapping.op      = ToneMapOperator::ACES_FILMIC;  // 0
        cfg.tone_mapping.exposure   = 1.0f;
        cfg.tone_mapping.saturation = 1.05f;
        cfg.vignette.enabled   = true;
        cfg.vignette.intensity = 0.35f;
        cfg.color_grading.enabled  = true;

        PostProcessChain chain = build_post_process_chain(
            cfg, "shaders", 1000, 500, /*srgb=*/false, /*lut=*/true);

        ExtractPC ex = read_pc<ExtractPC>(chain.passes[0]);
        PT_ASSERT(feq(ex.threshold, 0.75f), "extract: threshold = bloom.threshold");
        PT_ASSERT(feq(ex.soft_threshold, 0.5f), "extract: soft_threshold passed");

        BlurPC bh = read_pc<BlurPC>(chain.passes[1]);
        PT_ASSERT(feq(bh.dir_x, 1.0f / 1000.0f), "blur H: dir_x = 1/w");
        PT_ASSERT(feq(bh.dir_y, 0.0f),           "blur H: dir_y = 0");
        PT_ASSERT(feq(bh.radius, 3.0f),          "blur H: radius = bloom.radius");

        BlurPC bv = read_pc<BlurPC>(chain.passes[2]);
        PT_ASSERT(feq(bv.dir_x, 0.0f),           "blur V: dir_x = 0");
        PT_ASSERT(feq(bv.dir_y, 1.0f / 500.0f),  "blur V: dir_y = 1/h");

        GradePC gr = read_pc<GradePC>(chain.passes[3]);
        PT_ASSERT(feq(gr.bloom_intensity, 0.55f), "grade: bloom_intensity");
        PT_ASSERT_OP(gr.tonemap_op, ==, uint32_t{0}, "grade: tonemap_op = ACES");
        PT_ASSERT(feq(gr.exposure, 1.0f),        "grade: exposure");
        PT_ASSERT(feq(gr.gamma, 2.2f),           "grade: gamma (non-srgb)");
        PT_ASSERT(feq(gr.saturation, 1.05f),     "grade: saturation");
        PT_ASSERT(feq(gr.vignette_intensity, 0.35f), "grade: vignette on");
        PT_ASSERT(feq(gr.lut_intensity, 1.0f),   "grade: lut on (cg+lut loaded)");
    }

    // 4. 挙動保存 — 無効エフェクトが旧実装と同じく恒等へ縮退する。
    //    bloom off → extract threshold 1e9 (何も抽出しない) + radius 1.0、
    //    tonemap off → op=4 (LINEAR_CLAMP) + exposure 1.0、
    //    vignette off → vignette_intensity 0、 cg off → lut_intensity 0。
    {
        PostProcessConfig cfg;
        cfg.bloom.enabled        = false;
        cfg.tone_mapping.enabled = false;
        cfg.vignette.enabled     = false;
        cfg.color_grading.enabled = false;

        PostProcessChain chain = build_post_process_chain(
            cfg, "shaders", 800, 600, false, /*lut=*/true);

        ExtractPC ex = read_pc<ExtractPC>(chain.passes[0]);
        PT_ASSERT(ex.threshold > 1.0e8f, "bloom off: extract threshold ~1e9");

        BlurPC bh = read_pc<BlurPC>(chain.passes[1]);
        PT_ASSERT(feq(bh.radius, 1.0f), "bloom off: blur radius collapses to 1.0");

        GradePC gr = read_pc<GradePC>(chain.passes[3]);
        PT_ASSERT(feq(gr.bloom_intensity, 0.0f),    "bloom off: intensity 0");
        PT_ASSERT_OP(gr.tonemap_op, ==, uint32_t{4}, "tonemap off: op = LINEAR_CLAMP");
        PT_ASSERT(feq(gr.exposure, 1.0f),           "tonemap off: exposure 1.0");
        PT_ASSERT(feq(gr.saturation, 1.0f),         "tonemap off: saturation 1.0");
        PT_ASSERT(feq(gr.vignette_intensity, 0.0f), "vignette off: intensity 0");
        PT_ASSERT(feq(gr.lut_intensity, 0.0f),      "cg off: lut_intensity 0");
    }

    // 4b. lut が読めない (lut_loaded=false) → ColorGrading 有効でも lut 0。
    {
        PostProcessConfig cfg;
        cfg.color_grading.enabled = true;
        PostProcessChain chain = build_post_process_chain(
            cfg, "shaders", 640, 480, false, /*lut=*/false);
        GradePC gr = read_pc<GradePC>(chain.passes[3]);
        PT_ASSERT(feq(gr.lut_intensity, 0.0f),
                  "cg on but lut missing: lut_intensity 0");
    }

    // 4c. sRGB swapchain → grade の gamma が 1.0 に倒れる (二重ガンマ回避)。
    {
        PostProcessConfig cfg;
        cfg.tone_mapping.enabled = true;
        cfg.tone_mapping.gamma   = 2.2f;
        PostProcessChain chain = build_post_process_chain(
            cfg, "shaders", 640, 480, /*srgb=*/true, false);
        GradePC gr = read_pc<GradePC>(chain.passes[3]);
        PT_ASSERT(feq(gr.gamma, 1.0f), "srgb output: gamma collapses to 1.0");
    }

    // 5. refresh は pass 構造を変えず push_data だけ詰め直す。
    {
        PostProcessConfig cfg;
        cfg.bloom.enabled   = true;
        cfg.bloom.threshold = 0.5f;
        PostProcessChain chain = build_post_process_chain(
            cfg, "shaders", 1000, 1000, false, false);

        // config を変えて refresh — pass 数は不変、 push 値は更新される。
        cfg.bloom.threshold = 0.9f;
        refresh_post_process_chain(chain, cfg, 2000, 1000, false, false);

        PT_ASSERT_OP(chain.passes.size(), ==, size_t{4},
                     "refresh keeps 4 passes");
        ExtractPC ex = read_pc<ExtractPC>(chain.passes[0]);
        PT_ASSERT(feq(ex.threshold, 0.9f), "refresh updates extract threshold");
        BlurPC bh = read_pc<BlurPC>(chain.passes[1]);
        PT_ASSERT(feq(bh.dir_x, 1.0f / 2000.0f),
                  "refresh updates blur texel step to new width");
    }

    // 6. extra pass は組み込み 4 pass のあとに追加される (任意 pass 挿入)。
    //    組み込み名 (dof / ssao_apply / motion_blur / fxaa / bloom_* /
    //    color_grade) は refresh が push_data を管理する予約名なので、
    //    ホスト独自 pass には使わないこと。
    {
        PostProcessConfig cfg;
        PostProcessPassDef custom;
        custom.name     = "host_custom_fx";
        custom.vert_spv = "shaders/fullscreen_quad.vert.spv";
        custom.frag_spv = "shaders/host_custom_fx.frag.spv";
        custom.inputs   = {kPostProcessOutputTarget};
        custom.output   = kPostProcessOutputTarget;

        PostProcessChain chain = build_post_process_chain(
            cfg, "shaders", 800, 600, false, false, /*extra=*/{custom});
        PT_ASSERT_OP(chain.passes.size(), ==, size_t{5},
                     "extra pass appended after built-in 4");
        PT_ASSERT(chain.passes[4].name == "host_custom_fx",
                  "extra pass at the tail");

        // refresh は extra pass を触らない (push_data 空のまま)。
        refresh_post_process_chain(chain, cfg, 800, 600, false, false);
        const PostProcessPassDef* fx = find_pass(chain, "host_custom_fx");
        PT_ASSERT(fx != nullptr && fx->push_data.empty(),
                  "refresh leaves extra pass push_data untouched");
    }

    // 7. DoF 有効時はチェーン先頭へ dof pass が途中挿入され、 後段の scene
    //    入力が dof 出力 (pp_dof) へ配線替えされる。
    {
        PostProcessConfig cfg;
        cfg.depth_of_field.enabled        = true;
        cfg.depth_of_field.focus_distance = 12.0f;
        cfg.depth_of_field.bokeh_radius   = 3.0f;

        PostProcessChain chain = build_post_process_chain(
            cfg, "shaders", 1600, 900, false, false);

        PT_ASSERT_OP(chain.passes.size(), ==, size_t{5}, "dof chain: 5 passes");
        PT_ASSERT(chain.passes[0].name == "dof", "dof chain: dof first");
        PT_ASSERT(chain.passes[0].inputs[0] == kPostProcessSceneTarget,
                  "dof chain: dof reads scene");
        PT_ASSERT(chain.passes[0].inputs[1] == kPostProcessDepthTarget,
                  "dof chain: dof reads depth");
        PT_ASSERT(chain.passes[0].output == "pp_dof", "dof chain: dof -> pp_dof");
        PT_ASSERT(chain.passes[1].inputs[0] == "pp_dof",
                  "dof chain: extract rewired to pp_dof");
        PT_ASSERT(chain.passes[4].inputs[0] == "pp_dof",
                  "dof chain: grade rewired to pp_dof");
        PT_ASSERT_OP(chain.intermediate_names.size(), ==, size_t{3},
                     "dof chain: pp_dof + ping + pong");

        // push constant: focus_distance が先頭 float、 texel が 1/w, 1/h。
        struct DofPC {
            float    focus_distance, focus_range, bokeh_radius;
            float    near_start, near_end, far_start, far_end;
            uint32_t sample_count;
            float    texel_x, texel_y, pad0, pad1;
        };
        DofPC pc = read_pc<DofPC>(chain.passes[0]);
        PT_ASSERT(feq(pc.focus_distance, 12.0f), "dof pc: focus distance");
        PT_ASSERT(feq(pc.bokeh_radius, 3.0f),    "dof pc: bokeh radius");
        PT_ASSERT(feq(pc.texel_x, 1.0f / 1600.0f), "dof pc: texel x");
        PT_ASSERT(feq(pc.texel_y, 1.0f / 900.0f),  "dof pc: texel y");

        // refresh が dof の push_data も詰め直す (構造は不変)。
        cfg.depth_of_field.focus_distance = 20.0f;
        refresh_post_process_chain(chain, cfg, 1600, 900, false, false);
        pc = read_pc<DofPC>(chain.passes[0]);
        PT_ASSERT(feq(pc.focus_distance, 20.0f), "dof pc: refresh updates focus");
        PT_ASSERT_OP(chain.passes.size(), ==, size_t{5},
                     "refresh keeps dof chain structure");
    }

    // 8. チェーン途中編集 API — anchor 基準の挿入 / 削除 / 配線替え。
    {
        PostProcessConfig cfg;
        PostProcessChain chain = build_post_process_chain(
            cfg, "shaders", 800, 600, false, false);

        // blur H の後へ SSAO 風の pass を途中挿入。 新しい中間ターゲット名は
        // intermediate_names へ自動反映される。
        PostProcessPassDef ssao;
        ssao.name     = "ssao";
        ssao.vert_spv = "shaders/fullscreen_quad.vert.spv";
        ssao.frag_spv = "shaders/ssao.frag.spv";
        ssao.inputs   = {kPostProcessSceneTarget, kPostProcessDepthTarget};
        ssao.output   = "pp_ssao";
        PT_ASSERT(insert_post_process_pass(chain, ssao, "bloom_blur_h",
                                           PassInsertWhere::AFTER),
                  "chain edit: insert after blur H");
        PT_ASSERT_OP(find_post_process_pass(chain, "ssao"), ==, 2,
                     "chain edit: ssao at index 2 (after blur H)");
        bool has_ssao_target = false;
        for (const auto& n : chain.intermediate_names)
            if (n == "pp_ssao") has_ssao_target = true;
        PT_ASSERT(has_ssao_target, "chain edit: pp_ssao auto-collected");

        // 同名 / 不在 anchor は拒否 (チェーン不変)。
        PT_ASSERT(!insert_post_process_pass(chain, ssao, "color_grade",
                                            PassInsertWhere::BEFORE),
                  "chain edit: duplicate name rejected");
        PT_ASSERT(!insert_post_process_pass(chain, PostProcessPassDef{"x"},
                                            "no_such_pass",
                                            PassInsertWhere::BEFORE),
                  "chain edit: unknown anchor rejected");

        // 配線替え: grade の scene 入力を ssao 出力へ。
        PT_ASSERT(rebind_post_process_input(chain, "color_grade",
                                            kPostProcessSceneTarget, "pp_ssao"),
                  "chain edit: rebind grade input");
        const PostProcessPassDef* gr = find_pass(chain, "color_grade");
        PT_ASSERT(gr != nullptr && gr->inputs[0] == "pp_ssao",
                  "chain edit: grade reads pp_ssao");
        PT_ASSERT(!rebind_post_process_input(chain, "color_grade",
                                             "not_an_input", "x"),
                  "chain edit: rebind unknown input rejected");

        // 削除: ssao を抜くと pass 数が戻る。 pp_ssao はまだ grade が読むので
        // intermediate には残る。
        PT_ASSERT(remove_post_process_pass(chain, "ssao"),
                  "chain edit: remove ssao");
        PT_ASSERT_OP(chain.passes.size(), ==, size_t{4},
                     "chain edit: back to 4 passes");
        has_ssao_target = false;
        for (const auto& n : chain.intermediate_names)
            if (n == "pp_ssao") has_ssao_target = true;
        PT_ASSERT(has_ssao_target,
                  "chain edit: target kept while grade still reads it");

        // grade の入力を scene へ戻すと pp_ssao は intermediate から消える。
        PT_ASSERT(rebind_post_process_input(chain, "color_grade", "pp_ssao",
                                            kPostProcessSceneTarget),
                  "chain edit: rebind back to scene");
        has_ssao_target = false;
        for (const auto& n : chain.intermediate_names)
            if (n == "pp_ssao") has_ssao_target = true;
        PT_ASSERT(!has_ssao_target, "chain edit: unused target dropped");

        PT_ASSERT(!remove_post_process_pass(chain, "ssao"),
                  "chain edit: double removal rejected");
    }

    // 9. SSAO 有効 — scene 直後 (bloom 前) に挿入され、 後段が pp_ssao を読む。
    {
        PostProcessConfig cfg;
        cfg.ssao.enabled      = true;
        cfg.ssao.radius       = 10.0f;
        cfg.ssao.sample_count = 16;
        cfg.ssao.intensity    = 0.8f;

        PostProcessChain chain = build_post_process_chain(
            cfg, "shaders", 1920, 1080, false, false);

        PT_ASSERT_OP(chain.passes.size(), ==, size_t{5}, "ssao chain: 5 passes");
        PT_ASSERT(chain.passes[0].name == "ssao_apply", "ssao chain: ssao first");
        PT_ASSERT(chain.passes[0].inputs[0] == kPostProcessSceneTarget,
                  "ssao chain: reads scene");
        PT_ASSERT(chain.passes[0].inputs[1] == kPostProcessDepthTarget,
                  "ssao chain: reads depth");
        PT_ASSERT(chain.passes[0].output == "pp_ssao", "ssao chain: -> pp_ssao");
        PT_ASSERT(chain.passes[1].inputs[0] == "pp_ssao",
                  "ssao chain: extract rewired");
        PT_ASSERT(chain.passes[4].inputs[0] == "pp_ssao",
                  "ssao chain: grade rewired");

        SsaoPC pc = read_pc<SsaoPC>(chain.passes[0]);
        PT_ASSERT(feq(pc.radius_px, 10.0f),   "ssao pc: radius");
        PT_ASSERT(feq(pc.intensity, 0.8f),    "ssao pc: intensity");
        PT_ASSERT_OP(pc.sample_count, ==, uint32_t{16}, "ssao pc: samples");
        PT_ASSERT(feq(pc.texel_x, 1.0f / 1920.0f), "ssao pc: texel x");

        // refresh で intensity が更新される。 disabled にすると恒等 (0)。
        cfg.ssao.intensity = 0.0f;
        refresh_post_process_chain(chain, cfg, 1920, 1080, false, false);
        pc = read_pc<SsaoPC>(chain.passes[0]);
        PT_ASSERT(feq(pc.intensity, 0.0f), "ssao pc: refresh updates intensity");
    }

    // 10. Motion blur — 行列未確定 (matrix_valid=false) は valid=0 で素通し、
    //     refresh がホスト更新の行列を毎フレーム詰め直す。
    {
        PostProcessConfig cfg;
        cfg.motion_blur.enabled = true;   // まだ matrix_valid = false

        PostProcessChain chain = build_post_process_chain(
            cfg, "shaders", 800, 600, false, false);

        PT_ASSERT_OP(chain.passes.size(), ==, size_t{5}, "mblur chain: 5 passes");
        PT_ASSERT(chain.passes[0].name == "motion_blur", "mblur chain: first");
        PT_ASSERT(chain.passes[0].output == "pp_mblur", "mblur chain: -> pp_mblur");

        MotionBlurPC pc = read_pc<MotionBlurPC>(chain.passes[0]);
        PT_ASSERT_OP(pc.valid, ==, uint32_t{0}, "mblur pc: invalid until host sets matrix");
        PT_ASSERT(feq(pc.reproj[0], 1.0f), "mblur pc: identity default");

        cfg.motion_blur.matrix_valid = true;
        cfg.motion_blur.reproj_matrix[12] = 0.25f;   // 平行移動成分
        refresh_post_process_chain(chain, cfg, 800, 600, false, false);
        pc = read_pc<MotionBlurPC>(chain.passes[0]);
        PT_ASSERT_OP(pc.valid, ==, uint32_t{1}, "mblur pc: valid after host update");
        PT_ASSERT(feq(pc.reproj[12], 0.25f), "mblur pc: refresh carries matrix");
    }

    // 11. FXAA — grade の出力が pp_ldr へ差し替わり、 fxaa が末尾で
    //     pp_ldr → __output__ を張る (LDR で走る)。
    {
        PostProcessConfig cfg;
        cfg.fxaa.enabled = true;

        PostProcessChain chain = build_post_process_chain(
            cfg, "shaders", 1280, 720, false, false);

        PT_ASSERT_OP(chain.passes.size(), ==, size_t{5}, "fxaa chain: 5 passes");
        const PostProcessPassDef* gr = find_pass(chain, "color_grade");
        PT_ASSERT(gr != nullptr && gr->output == "pp_ldr",
                  "fxaa chain: grade -> pp_ldr");
        PT_ASSERT(chain.passes[4].name == "fxaa", "fxaa chain: fxaa last");
        PT_ASSERT(chain.passes[4].inputs[0] == "pp_ldr",
                  "fxaa chain: fxaa reads pp_ldr");
        PT_ASSERT(chain.passes[4].output == kPostProcessOutputTarget,
                  "fxaa chain: fxaa writes swapchain");

        FxaaPC pc = read_pc<FxaaPC>(chain.passes[4]);
        PT_ASSERT(feq(pc.edge_threshold, 0.166f), "fxaa pc: edge threshold");
        PT_ASSERT(feq(pc.subpix_quality, 0.75f),  "fxaa pc: subpix quality");
    }

    // 12. CA / film grain は grade へ統合 (pass を増やさない)。
    //     無効時は 0 へ縮退、 refresh が grain seed を運ぶ。
    {
        PostProcessConfig cfg;
        cfg.chromatic_aberration.enabled   = true;
        cfg.chromatic_aberration.intensity = 0.6f;
        cfg.film_grain.enabled   = true;
        cfg.film_grain.intensity = 0.4f;
        cfg.film_grain.seed      = 7.0f;

        PostProcessChain chain = build_post_process_chain(
            cfg, "shaders", 800, 600, false, false);
        PT_ASSERT_OP(chain.passes.size(), ==, size_t{4},
                     "ca/grain: no extra pass");

        GradePC gr = read_pc<GradePC>(chain.passes[3]);
        PT_ASSERT(feq(gr.ca_intensity, 0.6f),    "grade pc: ca intensity");
        PT_ASSERT(feq(gr.grain_intensity, 0.4f), "grade pc: grain intensity");
        PT_ASSERT(feq(gr.grain_seed, 7.0f),      "grade pc: grain seed");

        cfg.film_grain.seed = 8.0f;
        cfg.chromatic_aberration.enabled = false;
        refresh_post_process_chain(chain, cfg, 800, 600, false, false);
        gr = read_pc<GradePC>(chain.passes[3]);
        PT_ASSERT(feq(gr.grain_seed, 8.0f),   "grade pc: seed advances via refresh");
        PT_ASSERT(feq(gr.ca_intensity, 0.0f), "grade pc: ca off -> 0");
    }

    // 13. 全部盛り — 設計書 §2.1 の順序:
    //     dof → ssao → mblur → extract → blurH → blurV → grade → fxaa。
    {
        PostProcessConfig cfg;
        cfg.depth_of_field.enabled = true;
        cfg.ssao.enabled           = true;
        cfg.motion_blur.enabled    = true;
        cfg.fxaa.enabled           = true;

        PostProcessChain chain = build_post_process_chain(
            cfg, "shaders", 1920, 1080, false, false);

        PT_ASSERT_OP(chain.passes.size(), ==, size_t{8}, "full chain: 8 passes");
        PT_ASSERT(chain.passes[0].name == "dof",          "full chain order 0");
        PT_ASSERT(chain.passes[1].name == "ssao_apply",   "full chain order 1");
        PT_ASSERT(chain.passes[2].name == "motion_blur",  "full chain order 2");
        PT_ASSERT(chain.passes[3].name == "bloom_extract","full chain order 3");
        PT_ASSERT(chain.passes[7].name == "fxaa",         "full chain order 7");

        // 配線: dof → ssao → mblur と数珠つなぎ、 extract / grade は最後の
        // 挿入 pass (pp_mblur) を読む。
        PT_ASSERT(chain.passes[1].inputs[0] == "pp_dof",   "full: ssao reads dof");
        PT_ASSERT(chain.passes[2].inputs[0] == "pp_ssao",  "full: mblur reads ssao");
        PT_ASSERT(chain.passes[3].inputs[0] == "pp_mblur", "full: extract reads mblur");
        const PostProcessPassDef* gr = find_pass(chain, "color_grade");
        PT_ASSERT(gr != nullptr && gr->inputs[0] == "pp_mblur",
                  "full: grade reads mblur");

        // 中間ターゲット: dof/ssao/mblur/ping/pong/ldr = 6。
        PT_ASSERT_OP(chain.intermediate_names.size(), ==, size_t{6},
                     "full chain: 6 intermediates");
    }

    // 14. history 入力名ヘルパー (phase 2)。
    {
        PT_ASSERT(history_input_name("pp_taa") == "__history:pp_taa__",
                  "history name composed");
        PT_ASSERT(parse_history_input("__history:pp_taa__") == "pp_taa",
                  "history name parsed");
        PT_ASSERT(parse_history_input("pp_taa").empty(),
                  "plain target is not history");
        PT_ASSERT(parse_history_input("__history:__").empty(),
                  "empty source rejected");
    }

    // 15. TAA — mblur 後に挿入、 history 入力を持ち、 FXAA を抑制する。
    {
        PostProcessConfig cfg;
        cfg.taa.enabled  = true;
        cfg.fxaa.enabled = true;   // TAA 優先で外れる

        PostProcessChain chain = build_post_process_chain(
            cfg, "shaders", 1920, 1080, false, false);

        PT_ASSERT_OP(chain.passes.size(), ==, size_t{5},
                     "taa chain: 5 passes (fxaa suppressed)");
        PT_ASSERT(chain.passes[0].name == "taa", "taa chain: taa first");
        PT_ASSERT(chain.passes[0].inputs[0] == kPostProcessSceneTarget,
                  "taa reads scene");
        PT_ASSERT(chain.passes[0].inputs[1] == kPostProcessDepthTarget,
                  "taa reads depth");
        PT_ASSERT(chain.passes[0].inputs[2] == "__history:pp_taa__",
                  "taa reads its own history");
        PT_ASSERT(chain.passes[0].output == "pp_taa", "taa -> pp_taa");
        PT_ASSERT(find_pass(chain, "fxaa") == nullptr,
                  "fxaa suppressed while taa is on");
        const PostProcessPassDef* gr = find_pass(chain, "color_grade");
        PT_ASSERT(gr != nullptr && gr->output == kPostProcessOutputTarget,
                  "grade writes swapchain (no fxaa)");

        // history 名は中間ターゲットに数えない (pp_taa 自体は数える)。
        bool has_taa = false, has_history = false;
        for (const auto& n : chain.intermediate_names) {
            if (n == "pp_taa") has_taa = true;
            if (n.rfind("__history:", 0) == 0) has_history = true;
        }
        PT_ASSERT(has_taa, "pp_taa is an intermediate");
        PT_ASSERT(!has_history, "history names are not intermediates");

        // valid ゲート: matrix/history が立つまで 0。
        TaaPC pc = read_pc<TaaPC>(chain.passes[0]);
        PT_ASSERT_OP(pc.valid, ==, uint32_t{0}, "taa invalid until host seeds");
        cfg.taa.matrix_valid  = true;
        cfg.taa.history_valid = true;
        cfg.taa.jitter_x      = 0.25f;
        refresh_post_process_chain(chain, cfg, 1920, 1080, false, false);
        pc = read_pc<TaaPC>(chain.passes[0]);
        PT_ASSERT_OP(pc.valid, ==, uint32_t{1}, "taa valid after host update");
        PT_ASSERT(feq(pc.jitter_x, 0.25f), "taa jitter carried by refresh");
    }

    // 16. SSR — ssao 後 / mblur 前に挿入、 proj パラメータを運ぶ。
    {
        PostProcessConfig cfg;
        cfg.ssao.enabled        = true;
        cfg.ssr.enabled         = true;
        cfg.motion_blur.enabled = true;
        cfg.ssr.proj_xx         = 1.5f;
        cfg.ssr.max_steps       = 24;

        PostProcessChain chain = build_post_process_chain(
            cfg, "shaders", 1280, 720, false, false);

        PT_ASSERT_OP(chain.passes.size(), ==, size_t{7},
                     "ssao+ssr+mblur chain: 7 passes");
        PT_ASSERT(chain.passes[0].name == "ssao_apply", "order: ssao first");
        PT_ASSERT(chain.passes[1].name == "ssr",        "order: ssr second");
        PT_ASSERT(chain.passes[2].name == "motion_blur","order: mblur third");
        PT_ASSERT(chain.passes[1].inputs[0] == "pp_ssao", "ssr reads ssao out");
        PT_ASSERT(chain.passes[2].inputs[0] == "pp_ssr",  "mblur reads ssr out");

        SsrPC pc = read_pc<SsrPC>(chain.passes[1]);
        PT_ASSERT(feq(pc.proj_xx, 1.5f),   "ssr pc: proj_xx");
        PT_ASSERT_OP(pc.max_steps, ==, uint32_t{24}, "ssr pc: steps");
        PT_ASSERT(feq(pc.intensity, 0.6f), "ssr pc: default intensity");
    }

    // 17. Auto exposure — 計測 (1x1 viewport) + 適用の 2 pass、
    //     時間適応 blend が delta_seconds を反映する。
    {
        PostProcessConfig cfg;
        cfg.hdr.auto_exposure   = true;
        cfg.hdr.adaptation_rate = 2.0f;
        cfg.hdr.delta_seconds   = 0.5f;   // blend = 1 - e^-1 ≈ 0.632

        PostProcessChain chain = build_post_process_chain(
            cfg, "shaders", 800, 600, false, false);

        PT_ASSERT_OP(chain.passes.size(), ==, size_t{6},
                     "auto-exposure chain: 6 passes");
        PT_ASSERT(chain.passes[0].name == "exposure_measure", "measure first");
        PT_ASSERT(chain.passes[1].name == "exposure_apply",   "apply second");
        PT_ASSERT_OP(chain.passes[0].viewport_w, ==, uint32_t{1},
                     "measure runs at 1x1 viewport");
        PT_ASSERT(chain.passes[0].inputs[1] == "__history:pp_exposure__",
                  "measure reads its own history");
        PT_ASSERT(chain.passes[1].inputs[1] == "pp_exposure",
                  "apply reads measured luminance");
        PT_ASSERT(chain.passes[1].output == "pp_exposed",
                  "apply -> pp_exposed");
        PT_ASSERT(chain.passes[2].inputs[0] == "pp_exposed",
                  "extract reads exposed scene");

        ExposureMeasurePC mpc = read_pc<ExposureMeasurePC>(chain.passes[0]);
        PT_ASSERT(feq(mpc.blend, 1.0f - std::exp(-1.0f), 1e-3f),
                  "measure blend = 1 - exp(-dt*rate)");
        ExposureApplyPC apc = read_pc<ExposureApplyPC>(chain.passes[1]);
        PT_ASSERT(feq(apc.key, 0.18f), "apply key = hdr.key");

        // 無効へ倒すと apply は key 0 で恒等縮退 (refresh 経由)。
        cfg.hdr.auto_exposure = false;
        refresh_post_process_chain(chain, cfg, 800, 600, false, false);
        apc = read_pc<ExposureApplyPC>(chain.passes[1]);
        PT_ASSERT(feq(apc.key, 0.0f), "apply key collapses to 0 when off");
    }

    return report("unit_postprocess_chain_test");
}
