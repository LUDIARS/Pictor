// unit_compiled_graph_wiring_test — 系統B compile → execute_compiled 配線の
// headless 統合テスト (review/2026-06-11 D-2 / spec/pipeline-system-b-config.md §3.5)。
//
// 実 GPU (VkDevice) は使わない。 `PipelineCompiler::compile()` は
// `device == VK_NULL_HANDLE` の headless モードで graph **構造** (pass 順序 /
// pass_type / flags / clear / debug_name) を決定的に組み、
// `execute_compiled()` は render_pass / framebuffer が VK_NULL_HANDLE の pass
// を「record callback のみ」経路で回す — record 内容の検証までを対象とする
// (vkCmdBeginRenderPass 等の実発行は KS / Custos の統合テスト側)。
//
// 検証対象:
//   1. PipelineCompiler::compile() の headless 構造 (順序 / flags / clear)
//   2. execute_compiled() が pass ごとに record を 1 回・宣言順で呼ぶこと
//   3. CompiledPathDriver の engage / recompile / disengage ライフサイクル
//   4. CompiledBatchRecorder の統計 (skip / 未実装 pass の明示計上、 §7.1)
//   5. PictorRenderer::compile_render_graph() とプロファイル切替時の再 compile

// `pipeline_profile.h` (and the core enums it pulls in) must be processed
// before `vulkan.h` on Windows — see unit_pipeline_registries_test.cpp.
#include "pictor/pipeline/attachment_def.h"
#include "pictor/pipeline/pipeline_profile.h"

#include "pictor/core/pictor_renderer.h"
#include "pictor/pipeline/attachment_registry.h"
#include "pictor/pipeline/compiled_batch_recorder.h"
#include "pictor/pipeline/compiled_path_driver.h"
#include "pictor/pipeline/framebuffer_registry.h"
#include "pictor/pipeline/pipeline_compiler.h"
#include "pictor/pipeline/render_pass_registry.h"
#include "pictor/pipeline/render_pass_scheduler.h"
#include "test_common.h"

#include <cstring>
#include <vector>

using namespace pictor;
using pictor_test::g_failures;

#ifdef PICTOR_HAS_VULKAN

namespace {

// ---- fixture -------------------------------------------------------------

/// OPAQUE → TRANSPARENT → POST_PROCESS(host_recorded, swapchain) → COMPUTE
/// の 4 pass プロファイル。 built-in default attachments を使う。
PipelineProfileDef make_test_profile() {
    PipelineProfileDef def;
    def.profile_name = "WiringTest";

    auto atts = default_attachments();
    def.attachments.assign(atts.begin(), atts.end());

    RenderPassDef scene;
    scene.pass_name      = "ScenePass";
    scene.pass_type      = PassType::OPAQUE;
    scene.render_targets = {"scene_hdr_color", "scene_depth"};
    scene.sort_mode      = SortMode::FRONT_TO_BACK;
    scene.filter_mask    = 0x00FF;

    RenderPassDef transparent;
    transparent.pass_name      = "TransparentPass";
    transparent.pass_type      = PassType::TRANSPARENT;
    transparent.render_targets = {"scene_hdr_color", "scene_depth"};
    transparent.sort_mode      = SortMode::BACK_TO_FRONT;

    RenderPassDef composite;
    composite.pass_name      = "CompositePass";
    composite.pass_type      = PassType::POST_PROCESS;
    composite.render_targets = {"swapchain"};
    composite.sort_mode      = SortMode::NONE;
    composite.host_recorded  = true;

    RenderPassDef compute;
    compute.pass_name       = "GPUCullPass";
    compute.pass_type       = PassType::COMPUTE;
    compute.sort_mode       = SortMode::NONE;
    compute.gpu_driven_pass = true;

    def.render_passes = {scene, transparent, composite, compute};
    return def;
}

struct Registries {
    AttachmentRegistry  atts;
    RenderPassRegistry  rps;
    FramebufferRegistry fbs;

    explicit Registries(const PipelineProfileDef& profile) {
        atts.set_defs(profile.attachments);
        rps.set_passes(profile.render_passes);
        // fbs は initialize_vulkan しない (headless) — get() は常に
        // VK_NULL_HANDLE を返し、 execute_compiled は record-only 経路になる。
    }
};

// ---- 1. compile structure -------------------------------------------------

void test_compile_structure() {
    PipelineProfileDef profile = make_test_profile();
    Registries reg(profile);

    CompiledGraph g = PipelineCompiler::compile(profile, reg.atts, reg.rps,
                                                reg.fbs, VK_NULL_HANDLE,
                                                /*flight*/ 2, /*swap*/ 3);

    PT_ASSERT(g.passes.size() == 4, "4 compiled passes");
    PT_ASSERT(g.descriptor_pool == VK_NULL_HANDLE, "headless: no descriptor pool");
    if (g.passes.size() != 4) return;

    const CompiledPass& scene = g.passes[0];
    PT_ASSERT(scene.pass_type == static_cast<uint8_t>(PassType::OPAQUE), "pass0 OPAQUE");
    PT_ASSERT(scene.pass_id == 0, "pass0 stable id");
    PT_ASSERT(scene.debug_name && std::strcmp(scene.debug_name, "ScenePass") == 0,
              "pass0 debug_name");
    PT_ASSERT(scene.filter_mask == 0x00FF, "pass0 filter_mask preserved");
    PT_ASSERT(scene.sort_mode == static_cast<uint8_t>(SortMode::FRONT_TO_BACK),
              "pass0 sort_mode");
    PT_ASSERT(!scene.is_swapchain(), "pass0 not swapchain");
    PT_ASSERT(!scene.is_compute(), "pass0 not compute");
    PT_ASSERT(!scene.is_host_recorded(), "pass0 not host_recorded");
    // clear 値は AttachmentDef から pre-resolve される (color + depth の 2 個)。
    PT_ASSERT(scene.clear_count == 2, "pass0 two clear values");
    PT_ASSERT(scene.clear_values[1].depthStencil.depth == 1.0f, "pass0 depth clear 1.0");

    const CompiledPass& transparent = g.passes[1];
    PT_ASSERT(transparent.pass_type == static_cast<uint8_t>(PassType::TRANSPARENT),
              "pass1 TRANSPARENT");
    PT_ASSERT(transparent.pass_id == 1, "pass1 stable id");
    PT_ASSERT(transparent.sort_mode == static_cast<uint8_t>(SortMode::BACK_TO_FRONT),
              "pass1 sort_mode");

    const CompiledPass& composite = g.passes[2];
    PT_ASSERT(composite.pass_type == static_cast<uint8_t>(PassType::POST_PROCESS),
              "pass2 POST_PROCESS");
    PT_ASSERT(composite.is_swapchain(), "pass2 swapchain flag (targets 'swapchain')");
    PT_ASSERT(composite.is_host_recorded(), "pass2 host_recorded flag");
    // swapchain pass は per-image (swap=3)、 それ以外は per-flight (flight=2)。
    PT_ASSERT(composite.framebuffer_count == 3, "pass2 per-image fb slots");
    PT_ASSERT(scene.framebuffer_count == 2, "pass0 per-flight fb slots");

    const CompiledPass& compute = g.passes[3];
    PT_ASSERT(compute.pass_type == static_cast<uint8_t>(PassType::COMPUTE),
              "pass3 COMPUTE");
    PT_ASSERT(compute.pass_id == 3, "pass3 stable id");
    PT_ASSERT(compute.is_compute(), "pass3 compute flag");
}

// ---- 2. execute_compiled record order --------------------------------------

void test_execute_compiled_record_order() {
    PipelineProfileDef profile = make_test_profile();
    Registries reg(profile);

    RenderPassScheduler scheduler(profile);
    scheduler.set_compiled_graph(PipelineCompiler::compile(
        profile, reg.atts, reg.rps, reg.fbs, VK_NULL_HANDLE, 2, 3));
    PT_ASSERT(scheduler.has_compiled_graph(), "graph installed");

    // headless: render_pass / framebuffer が全て VK_NULL_HANDLE なので、
    // execute_compiled は Begin/End を発行せず record のみ呼ぶ (安全)。
    std::vector<uint8_t> seen_types;
    scheduler.execute_compiled(
        VK_NULL_HANDLE, /*flight*/ 0, /*image*/ 0,
        [&seen_types](VkCommandBuffer, const CompiledPass& cp,
                      uint32_t flight, uint32_t image) {
            PT_ASSERT(flight == 0 && image == 0, "frame indices forwarded");
            seen_types.push_back(cp.pass_type);
        });

    PT_ASSERT(seen_types.size() == 4, "record called once per pass");
    if (seen_types.size() == 4) {
        PT_ASSERT(seen_types[0] == static_cast<uint8_t>(PassType::OPAQUE),       "order[0]");
        PT_ASSERT(seen_types[1] == static_cast<uint8_t>(PassType::TRANSPARENT),  "order[1]");
        PT_ASSERT(seen_types[2] == static_cast<uint8_t>(PassType::POST_PROCESS), "order[2]");
        PT_ASSERT(seen_types[3] == static_cast<uint8_t>(PassType::COMPUTE),      "order[3]");
    }

    // take_compiled_graph で回収したら空 graph に戻り、 record は呼ばれない。
    CompiledGraph taken = scheduler.take_compiled_graph();
    PT_ASSERT(taken.passes.size() == 4, "taken graph carries passes");
    PT_ASSERT(!scheduler.has_compiled_graph(), "scheduler empty after take");

    uint32_t calls_after_take = 0;
    scheduler.execute_compiled(VK_NULL_HANDLE, 0, 0,
        [&calls_after_take](VkCommandBuffer, const CompiledPass&, uint32_t, uint32_t) {
            calls_after_take++;
        });
    PT_ASSERT(calls_after_take == 0, "no record after take");

    taken.shutdown(VK_NULL_HANDLE); // headless — pool 無し、 状態 clear のみ
}

// ---- 3. CompiledPathDriver lifecycle ---------------------------------------

void test_driver_engage_recompile_disengage() {
    PipelineProfileDef profile = make_test_profile();
    Registries reg(profile);
    RenderPassScheduler scheduler(profile);

    CompiledPathDriver driver;
    PT_ASSERT(!driver.engaged(), "initially disengaged");

    // registry 欠落は明示的に失敗する (§7.1: 無言フォールバック禁止)。
    CompiledPathDriver::Deps bad;
    bad.device = VK_NULL_HANDLE;
    PT_ASSERT(!driver.engage(bad, profile, scheduler), "missing deps -> false");
    PT_ASSERT(!driver.engaged(), "still disengaged after failed engage");

    CompiledPathDriver::Deps deps;
    deps.device                = VK_NULL_HANDLE; // headless
    deps.attachments           = &reg.atts;
    deps.render_passes         = &reg.rps;
    deps.framebuffers          = &reg.fbs;
    deps.flight_count          = 2;
    deps.swapchain_image_count = 3;

    PT_ASSERT(driver.engage(deps, profile, scheduler), "engage ok");
    PT_ASSERT(driver.engaged(), "engaged");
    PT_ASSERT(scheduler.compiled_graph().passes.size() == 4, "4-pass graph installed");

    // プロファイル切替: pass 構成の違う profile で recompile → graph 差し替え。
    PipelineProfileDef lite = PipelineProfileManager::create_lite_profile();
    PT_ASSERT(driver.recompile(lite, scheduler), "recompile ok");
    PT_ASSERT(scheduler.compiled_graph().passes.size() == lite.render_passes.size(),
              "graph re-compiled to Lite pass count");

    driver.disengage(scheduler);
    PT_ASSERT(!driver.engaged(), "disengaged");
    PT_ASSERT(!scheduler.has_compiled_graph(), "graph released from scheduler");
}

void test_pass_id_callback_table() {
    PipelineProfileDef profile = PipelineProfileManager::create_high_profile();
    Registries reg(profile);
    RenderPassScheduler scheduler(profile);
    scheduler.set_compiled_graph(PipelineCompiler::compile(
        profile, reg.atts, reg.rps, reg.fbs, VK_NULL_HANDLE, 2, 3));

    const uint16_t light_cull_id = scheduler.pass_id_of("LightCullPass");
    const uint16_t ssao_id = scheduler.pass_id_of("SSAOGen");
    PT_ASSERT(light_cull_id != CompiledPass::INVALID_PASS_ID, "LightCullPass id resolved");
    PT_ASSERT(ssao_id != CompiledPass::INVALID_PASS_ID, "SSAOGen id resolved");
    PT_ASSERT(light_cull_id != ssao_id, "compute passes have distinct ids");

    uint32_t light_cull_calls = 0;
    uint32_t ssao_calls = 0;
    uint32_t fallback_calls = 0;
    PT_ASSERT(scheduler.set_pass_record_callback(
        light_cull_id,
        [&light_cull_calls, light_cull_id](VkCommandBuffer, const CompiledPass& cp,
                                           uint32_t, uint32_t) {
            PT_ASSERT(cp.pass_id == light_cull_id, "LightCull callback id");
            light_cull_calls++;
        }), "register LightCull callback");
    PT_ASSERT(scheduler.set_pass_record_callback(
        ssao_id,
        [&ssao_calls, ssao_id](VkCommandBuffer, const CompiledPass& cp,
                               uint32_t, uint32_t) {
            PT_ASSERT(cp.pass_id == ssao_id, "SSAO callback id");
            ssao_calls++;
        }), "register SSAO callback");
    PT_ASSERT(!scheduler.set_pass_record_callback(
        CompiledPass::INVALID_PASS_ID, {}), "invalid pass id rejected");

    scheduler.execute_compiled(
        VK_NULL_HANDLE, 0, 0,
        [&fallback_calls](VkCommandBuffer, const CompiledPass&, uint32_t, uint32_t) {
            fallback_calls++;
        });

    PT_ASSERT_OP(light_cull_calls, ==, 1u, "LightCull callback once");
    PT_ASSERT_OP(ssao_calls, ==, 1u, "SSAO callback once");
    PT_ASSERT_OP(fallback_calls, ==,
                 static_cast<uint32_t>(profile.render_passes.size() - 2),
                 "fallback handles unregistered passes");
}

// ---- 4. CompiledBatchRecorder stats ----------------------------------------

/// GPU 実体を一切返さないスタブ (headless — vkCmd* を発行させない)。
class NullGpuSource : public IBatchGpuSource {
public:
    uint32_t resolve_calls = 0;
    uint64_t last_shader_key = 0;

    bool resolve(const RenderBatch&, uint64_t shader_key, PassType,
                 BatchGpuResources&) override {
        resolve_calls++;
        last_shader_key = shader_key;
        return false;
    }
};

void test_batch_recorder_stats() {
    NullGpuSource source;
    CompiledBatchRecorder recorder(source);

    std::vector<RenderBatch> batches(3);
    batches[0].shaderKey = 0xA1;
    batches[1].shaderKey = 0xA2;
    batches[2].shaderKey = 0xA3;
    recorder.begin_frame(&batches);

    // OPAQUE: バッチごとに resolve が呼ばれ、 実体なし → skip 計上・draw 0。
    CompiledPass opaque{};
    opaque.pass_type = static_cast<uint8_t>(PassType::OPAQUE);
    recorder.record(VK_NULL_HANDLE, opaque, 0, 0);
    PT_ASSERT(source.resolve_calls == 3, "resolve per batch");
    PT_ASSERT(source.last_shader_key == 0xA3, "shader key forwarded (no registry)");
    PT_ASSERT(recorder.stats().draw_calls == 0, "no draws without GPU resources");
    PT_ASSERT(recorder.stats().batches_skipped == 3, "skips counted");

    batches[0].transparency = 0;
    batches[1].transparency = 1;
    batches[2].transparency = 0;
    source.resolve_calls = 0;
    recorder.begin_frame(&batches);
    opaque.filter_mask = RenderBatchFilter::OPAQUE;
    recorder.record(VK_NULL_HANDLE, opaque, 0, 0);
    PT_ASSERT_OP(source.resolve_calls, ==, 2u, "opaque pass resolves opaque batches only");
    PT_ASSERT_OP(recorder.stats().batches_filtered, ==, 1u,
                 "opaque pass filters transparent batch");

    source.resolve_calls = 0;
    recorder.begin_frame(&batches);
    CompiledPass transparent{};
    transparent.pass_type = static_cast<uint8_t>(PassType::TRANSPARENT);
    transparent.filter_mask = RenderBatchFilter::TRANSPARENT;
    recorder.record(VK_NULL_HANDLE, transparent, 0, 0);
    PT_ASSERT_OP(source.resolve_calls, ==, 1u,
                 "transparent pass resolves transparent batches only");
    PT_ASSERT_OP(recorder.stats().batches_filtered, ==, 2u,
                 "transparent pass filters opaque batches");

    // SHADOW / DEPTH_ONLY: 未実装は無言 skip ではなく明示計上 (§7.1)。
    CompiledPass shadow{};
    shadow.pass_type = static_cast<uint8_t>(PassType::SHADOW);
    recorder.record(VK_NULL_HANDLE, shadow, 0, 0);
    CompiledPass depth{};
    depth.pass_type = static_cast<uint8_t>(PassType::DEPTH_ONLY);
    recorder.record(VK_NULL_HANDLE, depth, 0, 0);
    PT_ASSERT(recorder.stats().passes_unimplemented == 2, "unimplemented counted");

    // COMPUTE / POST_PROCESS は本 recorder の対象外として計上。
    CompiledPass post{};
    post.pass_type = static_cast<uint8_t>(PassType::POST_PROCESS);
    recorder.record(VK_NULL_HANDLE, post, 0, 0);
    PT_ASSERT(recorder.stats().passes_not_recorded == 1, "not-recorded counted");

    // begin_frame で統計はリセットされる。
    recorder.begin_frame(&batches);
    PT_ASSERT(recorder.stats().batches_skipped == 0, "stats reset on begin_frame");
}

// ---- 5. PictorRenderer wiring ----------------------------------------------

void test_renderer_wiring_and_profile_switch() {
    PipelineProfileDef standard = PipelineProfileManager::create_standard_profile();
    Registries reg(standard);

    PictorRenderer renderer;
    renderer.initialize(); // initial_profile = "Standard"

    PT_ASSERT(!renderer.has_compiled_render_graph(), "no graph before compile");

    bool ok = renderer.compile_render_graph(VK_NULL_HANDLE, reg.atts, reg.rps,
                                            reg.fbs, /*flight*/ 2, /*swap*/ 3);
    PT_ASSERT(ok, "compile_render_graph ok (headless)");
    PT_ASSERT(renderer.has_compiled_render_graph(), "graph installed");

    // プロファイル切替 → apply_profile 経由で自動再 compile (§3.5:
    // 再 compile はプロファイル切替時のみ。 毎フレームではない)。
    PipelineProfileDef lite = PipelineProfileManager::create_lite_profile();
    PT_ASSERT(renderer.set_profile("Lite"), "switch to Lite");
    PT_ASSERT(renderer.has_compiled_render_graph(), "graph survives profile switch");

    // 切替後の graph が Lite の pass 構成 (5 pass) を反映していることを、
    // record 呼び出し回数で観測する (headless record-only 経路)。
    uint32_t record_calls = 0;
    renderer.render_compiled(VK_NULL_HANDLE, 0, 0,
        [&record_calls](VkCommandBuffer, const CompiledPass&, uint32_t, uint32_t) {
            record_calls++;
        });
    PT_ASSERT(record_calls == lite.render_passes.size(),
              "record count matches Lite pass count after re-compile");

    renderer.release_render_graph();
    PT_ASSERT(!renderer.has_compiled_render_graph(), "released");

    renderer.shutdown();
}

} // anon

int main() {
    test_compile_structure();
    test_execute_compiled_record_order();
    test_driver_engage_recompile_disengage();
    test_pass_id_callback_table();
    test_batch_recorder_stats();
    test_renderer_wiring_and_profile_switch();
    return pictor_test::report("unit_compiled_graph_wiring_test");
}

#else // !PICTOR_HAS_VULKAN

// Vulkan 無しビルドでは compile 経路自体が存在しない (PipelineCompiler は
// 空 graph スタブ)。 テスト対象外として素通しする。
int main() {
    return pictor_test::report("unit_compiled_graph_wiring_test");
}

#endif // PICTOR_HAS_VULKAN
