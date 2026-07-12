/// Low / Mid / High 品質 3 段階プリセットと、 パイプライン途中編集 API の検証.
///
/// 対象:
///   - `PipelineProfileManager::create_low/mid/high_profile()` の構造
///     (Mid = Bloom 系ポスト、 High = DoF + Forward+ 相当の pass 列)
///   - `insert_render_pass_before/after()` / `remove_render_pass()` /
///     `insert_post_process_before/after()` / `remove_post_process()`
///   - プリセット → `build_post_process_config()` →
///     `build_post_process_chain()` の通し (High で DoF pass が途中挿入
///     されること)
///
/// Vulkan device は不要 (宣言データの検証のみ)。

#include "pictor/pipeline/pipeline_profile.h"
#include "pictor/postprocess/postprocess_chain.h"
#include "pictor/postprocess/postprocess_config_bridge.h"
#include "test_common.h"

using namespace pictor;
using namespace pictor_test;

int main() {
    // 1. Low — FORWARD 最小構成。 post は Tonemapping のみ。
    {
        PipelineProfileDef low = PipelineProfileManager::create_low_profile();
        PT_ASSERT(low.profile_name == "Low", "Low: name");
        PT_ASSERT(low.rendering_path == RenderingPath::FORWARD, "Low: FORWARD");
        PT_ASSERT(!low.gpu_driven_enabled, "Low: gpu driven off");
        PT_ASSERT_OP(low.render_passes.size(), ==, size_t{3},
                     "Low: 3 render passes (Opaque/Transparent/Post)");
        PT_ASSERT_OP(low.post_process_stack.size(), ==, size_t{1},
                     "Low: post = Tonemapping only");
        PT_ASSERT(low.post_process_stack[0].kind == PostProcessKind::TONE_MAPPING,
                  "Low: tonemapping kind normalized");
    }

    // 2. Mid — Low + Shadow/DepthPre、 post に Bloom / Vignette (Bloom 等が
    //    載るパイプライン)。
    {
        PipelineProfileDef mid = PipelineProfileManager::create_mid_profile();
        PT_ASSERT(mid.profile_name == "Mid", "Mid: name");
        PT_ASSERT(mid.rendering_path == RenderingPath::FORWARD, "Mid: FORWARD");

        // 途中挿入の結果: Shadow → DepthPre → Opaque の順。
        const int shadow = find_render_pass(mid, "ShadowPass");
        const int depth  = find_render_pass(mid, "DepthPrePass");
        const int opaque = find_render_pass(mid, "OpaquePass");
        PT_ASSERT(shadow >= 0 && depth >= 0 && opaque >= 0,
                  "Mid: Shadow/DepthPre/Opaque present");
        PT_ASSERT(shadow < depth && depth < opaque,
                  "Mid: Shadow -> DepthPre -> Opaque order");

        // post: Bloom は Tonemapping の前、 Vignette は後。
        const int bloom = find_post_process(mid, "Bloom");
        const int tm    = find_post_process(mid, "Tonemapping");
        const int vig   = find_post_process(mid, "Vignette");
        PT_ASSERT(bloom >= 0 && tm >= 0 && vig >= 0, "Mid: Bloom/TM/Vignette");
        PT_ASSERT(bloom < tm && tm < vig, "Mid: Bloom -> TM -> Vignette order");
        PT_ASSERT(mid.post_process_stack[static_cast<size_t>(bloom)].kind ==
                      PostProcessKind::BLOOM,
                  "Mid: Bloom kind normalized");
        PT_ASSERT(mid.shadow_config.filter_mode == ShadowFilterMode::PCF,
                  "Mid: PCF shadows");
    }

    // 3. High — Forward+ 相当 (DepthPre → LightCull → shading) + DoF。
    {
        PipelineProfileDef high = PipelineProfileManager::create_high_profile();
        PT_ASSERT(high.profile_name == "High", "High: name");
        PT_ASSERT(high.rendering_path == RenderingPath::FORWARD_PLUS,
                  "High: FORWARD_PLUS");

        // LightCullPass は DepthPrePass の直後 (Forward+ の depth prepass →
        // tiled light culling)。
        const int depth = find_render_pass(high, "DepthPrePass");
        const int cull  = find_render_pass(high, "LightCullPass");
        const int ssao  = find_render_pass(high, "SSAOGen");
        const int opaque = find_render_pass(high, "OpaquePass");
        PT_ASSERT(depth >= 0 && cull >= 0 && ssao >= 0, "High: FW+ passes present");
        PT_ASSERT_OP(cull, ==, depth + 1, "High: LightCull right after DepthPre");
        PT_ASSERT(ssao < opaque, "High: SSAO before shading");
        PT_ASSERT(high.render_passes[static_cast<size_t>(cull)].pass_type ==
                      PassType::COMPUTE,
                  "High: LightCull is a compute pass");

        // post: DoF が Bloom より前 (深度をチェーンへ捩じ込む)。
        const int dof   = find_post_process(high, "DepthOfField");
        const int bloom = find_post_process(high, "Bloom");
        PT_ASSERT(dof >= 0 && bloom >= 0, "High: DoF + Bloom present");
        PT_ASSERT(dof < bloom, "High: DoF before Bloom");
        const auto& dof_def = high.post_process_stack[static_cast<size_t>(dof)];
        PT_ASSERT(dof_def.kind == PostProcessKind::DEPTH_OF_FIELD,
                  "High: DoF kind normalized");
        PT_ASSERT(dof_def.enabled, "High: DoF enabled");
    }

    // 4. High プリセット → config → チェーンの通し: DoF pass がチェーン先頭へ
    //    途中挿入され、 後段が DoF 出力を読むよう配線替えされる。
    {
        PipelineProfileDef high = PipelineProfileManager::create_high_profile();
        PostProcessConfig cfg = build_post_process_config(high);
        PT_ASSERT(cfg.depth_of_field.enabled, "High config: DoF enabled");

        PostProcessChain chain = build_post_process_chain(
            cfg, "shaders", 1920, 1080, /*srgb=*/false, /*lut=*/false);
        PT_ASSERT_OP(chain.passes.size(), ==, size_t{5},
                     "High chain: dof + built-in 4");
        PT_ASSERT(chain.passes[0].name == "dof", "High chain: dof first");
        PT_ASSERT_OP(chain.passes[0].inputs.size(), ==, size_t{2},
                     "High chain: dof reads scene + depth");
        PT_ASSERT(chain.passes[0].inputs[0] == kPostProcessSceneTarget,
                  "High chain: dof input0 = scene");
        PT_ASSERT(chain.passes[0].inputs[1] == kPostProcessDepthTarget,
                  "High chain: dof input1 = depth");
        PT_ASSERT(chain.passes[1].inputs[0] == "pp_dof",
                  "High chain: extract rewired to dof output");
        PT_ASSERT(chain.passes[4].inputs[0] == "pp_dof",
                  "High chain: grade rewired to dof output");
    }

    // 5. Mid (DoF 無し) のチェーンは従来どおり 4 pass — 構造の後方互換。
    {
        PipelineProfileDef mid = PipelineProfileManager::create_mid_profile();
        PostProcessConfig cfg = build_post_process_config(mid);
        PT_ASSERT(!cfg.depth_of_field.enabled, "Mid config: DoF off");
        PostProcessChain chain = build_post_process_chain(
            cfg, "shaders", 1920, 1080, false, false);
        PT_ASSERT_OP(chain.passes.size(), ==, size_t{4},
                     "Mid chain: built-in 4 passes only");
        PT_ASSERT(chain.passes[0].inputs[0] == kPostProcessSceneTarget,
                  "Mid chain: extract reads scene directly");
    }

    // 6. render pass 途中編集 API — anchor 基準の挿入 / 削除。
    {
        PipelineProfileDef def = PipelineProfileManager::create_low_profile();

        RenderPassDef custom;
        custom.pass_name = "OutlinePass";
        custom.pass_type = PassType::CUSTOM;
        PT_ASSERT(insert_render_pass_after(def, "OpaquePass", custom),
                  "edit: insert after OpaquePass");
        PT_ASSERT_OP(find_render_pass(def, "OutlinePass"), ==,
                     find_render_pass(def, "OpaquePass") + 1,
                     "edit: OutlinePass sits right after OpaquePass");

        // 同名の再挿入は拒否 (チェーン不変)。
        PT_ASSERT(!insert_render_pass_before(def, "OpaquePass", custom),
                  "edit: duplicate pass name rejected");
        // 不在 anchor は拒否。
        PT_ASSERT(!insert_render_pass_after(def, "NoSuchPass", custom),
                  "edit: unknown anchor rejected");

        PT_ASSERT(remove_render_pass(def, "OutlinePass"), "edit: remove pass");
        PT_ASSERT_OP(find_render_pass(def, "OutlinePass"), ==, -1,
                     "edit: pass gone after removal");
        PT_ASSERT(!remove_render_pass(def, "OutlinePass"),
                  "edit: double removal rejected");
    }

    // 7. post-process 途中編集 API — kind 補完も確認。
    {
        PipelineProfileDef def = PipelineProfileManager::create_mid_profile();

        PostProcessDef dof;
        dof.name    = "DoF";  // エイリアス表記 -> kind 補完される
        dof.enabled = true;
        PT_ASSERT(insert_post_process_before(def, "Bloom", dof),
                  "edit: insert DoF before Bloom");
        const int at = find_post_process(def, "DoF");
        PT_ASSERT(at >= 0 && at < find_post_process(def, "Bloom"),
                  "edit: DoF sits before Bloom");
        PT_ASSERT(def.post_process_stack[static_cast<size_t>(at)].kind ==
                      PostProcessKind::DEPTH_OF_FIELD,
                  "edit: kind inferred from alias name");

        PT_ASSERT(remove_post_process(def, "DoF"), "edit: remove effect");
        PT_ASSERT(!insert_post_process_after(def, "NoSuchEffect", dof),
                  "edit: unknown post anchor rejected");
    }

    // 8. Manager: register_defaults が 3 段階 + 従来プリセットを提供する。
    {
        PipelineProfileManager mgr;
        mgr.register_defaults();
        PT_ASSERT(mgr.get_profile("Low")  != nullptr, "mgr: Low registered");
        PT_ASSERT(mgr.get_profile("Mid")  != nullptr, "mgr: Mid registered");
        PT_ASSERT(mgr.get_profile("High") != nullptr, "mgr: High registered");
        PT_ASSERT(mgr.get_profile("Standard") != nullptr, "mgr: Standard kept");
        PT_ASSERT(mgr.current_profile_name() == "Standard",
                  "mgr: active default unchanged (Standard)");
        PT_ASSERT(mgr.set_profile("High"), "mgr: switch to High");
        PT_ASSERT(mgr.current_profile_name() == "High", "mgr: High active");
    }

    return report("unit_pipeline_tiers_test");
}
