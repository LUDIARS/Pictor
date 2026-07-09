#include "pictor/pipeline/compiled_path_driver.h"
#include "pictor/pipeline/pipeline_compiler.h"
#include "pictor/pipeline/render_pass_scheduler.h"

#include <cstdio>

namespace pictor {

#ifdef PICTOR_HAS_VULKAN

bool CompiledPathDriver::engage(const Deps& deps,
                                const PipelineProfileDef& profile,
                                RenderPassScheduler& scheduler)
{
    // §7.1: 依存不足を無言フォールバックで通さない — engage 失敗は明示する。
    if (!deps.attachments || !deps.render_passes || !deps.framebuffers) {
        std::fprintf(stderr,
                     "[CompiledPathDriver] engage failed: registry deps missing "
                     "(attachments=%p render_passes=%p framebuffers=%p)\n",
                     static_cast<const void*>(deps.attachments),
                     static_cast<const void*>(deps.render_passes),
                     static_cast<const void*>(deps.framebuffers));
        return false;
    }

    deps_    = deps;
    engaged_ = true;

    if (!compile_and_install_(profile, scheduler)) {
        engaged_ = false;
        return false;
    }
    return true;
}

bool CompiledPathDriver::recompile(const PipelineProfileDef& profile,
                                   RenderPassScheduler& scheduler)
{
    if (!engaged_) {
        std::fprintf(stderr,
                     "[CompiledPathDriver] recompile called before engage()\n");
        return false;
    }
    return compile_and_install_(profile, scheduler);
}

void CompiledPathDriver::disengage(RenderPassScheduler& scheduler) {
    if (!engaged_) return;

    // scheduler へ move した graph を回収して descriptor pool を解放する。
    // (set_compiled_graph は所有権 move — 解放責任はこちらに残る契約。)
    CompiledGraph old = scheduler.take_compiled_graph();
    release_old_graph_(old);

    deps_    = Deps{};
    engaged_ = false;
}

void CompiledPathDriver::release_old_graph_(CompiledGraph& old) {
    if (old.passes.empty() && old.descriptor_pool == VK_NULL_HANDLE) {
        return;  // 解放対象なし — idle 待ちも不要
    }
    // descriptor pool は in-flight フレームが参照し得る (thermal auto-downgrade
    // 経由の recompile は描画中にも起こる)。破棄前に GPU idle を待つ。
    if (deps_.device != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(deps_.device);
    }
    old.shutdown(deps_.device);
}

bool CompiledPathDriver::compile_and_install_(const PipelineProfileDef& profile,
                                              RenderPassScheduler& scheduler)
{
    // 差し替え前に旧 graph のリソースを解放 (リークさせない — 全経路で解放)。
    CompiledGraph old = scheduler.take_compiled_graph();
    release_old_graph_(old);

    CompiledGraph g = PipelineCompiler::compile(profile,
                                                *deps_.attachments,
                                                *deps_.render_passes,
                                                *deps_.framebuffers,
                                                deps_.device,
                                                deps_.flight_count,
                                                deps_.swapchain_image_count);
    if (g.passes.empty()) {
        // compile 失敗 (詳細は PipelineCompiler が stderr 出力済み)。
        // 空 graph を install してしまうと execute_compiled が「何もせず成功」
        // に見えるため、 明示ログ + 未 install で返す (§7.1)。
        std::fprintf(stderr,
                     "[CompiledPathDriver] compile produced empty graph for "
                     "profile '%s' — compiled path NOT installed\n",
                     profile.profile_name.c_str());
        return false;
    }

    scheduler.set_compiled_graph(std::move(g));
    return true;
}

#endif // PICTOR_HAS_VULKAN

} // namespace pictor
