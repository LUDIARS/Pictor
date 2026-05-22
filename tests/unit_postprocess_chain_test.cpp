/// PostProcessChain — 組み込みエフェクト → 汎用チェーン畳み込みの検証.
///
/// `spec/rendering-extensibility-design.md` §6.3 (phase 2 項目1)。
/// VkPipeline 生成は Vulkan device を要するためここでは検証しない。
/// チェーン構造 (pass 数 / 入出力ターゲット名 / push constant バイト列) と、
/// 既存 4 エフェクト (Bloom / ToneMapping / Vignette / ColorGrading) の
/// 「無効時は恒等へ縮退」 という挙動保存をバイト単位で見る。

#include "pictor/postprocess/postprocess_chain.h"
#include "test_common.h"

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
};

template <typename T>
T read_pc(const PostProcessPassDef& p) {
    T out{};
    if (p.push_data.size() >= sizeof(T))
        std::memcpy(&out, p.push_data.data(), sizeof(T));
    return out;
}

bool feq(float a, float b) { return (a - b) < 1e-4f && (b - a) < 1e-4f; }

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
    {
        PostProcessConfig cfg;
        PostProcessPassDef fxaa;
        fxaa.name     = "fxaa";
        fxaa.vert_spv = "shaders/fullscreen_quad.vert.spv";
        fxaa.frag_spv = "shaders/fxaa.frag.spv";
        fxaa.inputs   = {kPostProcessOutputTarget};
        fxaa.output   = kPostProcessOutputTarget;

        PostProcessChain chain = build_post_process_chain(
            cfg, "shaders", 800, 600, false, false, /*extra=*/{fxaa});
        PT_ASSERT_OP(chain.passes.size(), ==, size_t{5},
                     "extra pass appended after built-in 4");
        PT_ASSERT(chain.passes[4].name == "fxaa", "extra pass at the tail");

        // refresh は extra pass を触らない (push_data 空のまま)。
        refresh_post_process_chain(chain, cfg, 800, 600, false, false);
        const PostProcessPassDef* fx = find_pass(chain, "fxaa");
        PT_ASSERT(fx != nullptr && fx->push_data.empty(),
                  "refresh leaves extra pass push_data untouched");
    }

    return report("unit_postprocess_chain_test");
}
