// unit_pipeline_registries_test — Phase 3 Step B の Vulkan 非依存部分を確認。
//
// Vulkan の VkImage / VkRenderPass / VkFramebuffer 生成は実 GPU が要るため
// 単体テストでは対象外 (KS / Custos の統合テストでカバー)。 ここでは:
//
// 1. AttachmentRegistry::set_defs / index_of / count / def / defs が
//    名前→index 解決と def 配列保持を正しく行う
// 2. RenderPassRegistry::set_passes / index_of / count / pass / passes が
//    パス名→index 解決を行う
// 3. INVALID_INDEX 戻り値が未登録名で返る

// `pipeline_profile.h` (and the core enums it pulls in) must be processed
// before `vulkan.h` on Windows — vulkan.h transitively brings <windows.h>
// which `#define OPAQUE` collides with `PassType::OPAQUE`. Same pattern as
// `postprocess_pipeline.h` / `gpu_timer.h`.
#include "pictor/pipeline/attachment_def.h"
#include "pictor/pipeline/pipeline_profile.h"

#include "pictor/pipeline/attachment_registry.h"
#include "pictor/pipeline/render_pass_registry.h"
#include "test_common.h"

#include <string>
#include <vector>

using namespace pictor;
using pictor_test::g_failures;

namespace {

void test_attachment_registry_index_resolution() {
    AttachmentRegistry reg;

    auto defs = default_attachments();
    std::vector<AttachmentDef> vec(defs.begin(), defs.end());
    reg.set_defs(vec);

    PT_ASSERT(reg.count() == 3, "count 3");
    PT_ASSERT(reg.index_of("scene_hdr_color") == 0, "hdr->0");
    PT_ASSERT(reg.index_of("scene_depth") == 1, "depth->1");
    PT_ASSERT(reg.index_of("swapchain") == 2, "swap->2");
    PT_ASSERT(reg.index_of("bogus") == AttachmentRegistry::INVALID_INDEX, "miss->INVALID");

    PT_ASSERT(reg.def(0).kind == AttachmentKind::COLOR, "def(0) kind");
    PT_ASSERT(reg.def(1).kind == AttachmentKind::DEPTH, "def(1) kind");
    PT_ASSERT(reg.def(2).kind == AttachmentKind::SWAPCHAIN_COLOR, "def(2) kind");

    PT_ASSERT(reg.defs().size() == 3, "defs() size");
}

void test_attachment_registry_set_defs_replaces() {
    AttachmentRegistry reg;
    auto defs = default_attachments();
    std::vector<AttachmentDef> vec(defs.begin(), defs.end());
    reg.set_defs(vec);
    PT_ASSERT(reg.count() == 3, "first 3");

    // Replace with a single def.
    AttachmentDef only;
    only.name   = "shadow_atlas";
    only.kind   = AttachmentKind::DEPTH;
    only.format = AttachmentFormat::D32_SFLOAT;
    std::vector<AttachmentDef> one = {only};
    reg.set_defs(one);
    PT_ASSERT(reg.count() == 1, "now 1");
    PT_ASSERT(reg.index_of("scene_hdr_color") == AttachmentRegistry::INVALID_INDEX, "old gone");
    PT_ASSERT(reg.index_of("shadow_atlas") == 0, "new present");
}

void test_render_pass_registry_index_resolution() {
    RenderPassRegistry reg;

    RenderPassDef a;
    a.pass_name = "Scene HDR";
    a.pass_type = PassType::OPAQUE;
    a.render_targets = {"scene_hdr_color", "scene_depth"};

    RenderPassDef b;
    b.pass_name = "Swapchain Composite";
    b.pass_type = PassType::POST_PROCESS;
    b.render_targets = {"swapchain"};

    std::vector<RenderPassDef> passes = {a, b};
    reg.set_passes(passes);

    PT_ASSERT(reg.count() == 2, "2 passes");
    PT_ASSERT(reg.index_of("Scene HDR") == 0, "Scene HDR -> 0");
    PT_ASSERT(reg.index_of("Swapchain Composite") == 1, "Composite -> 1");
    PT_ASSERT(reg.index_of("bogus") == RenderPassRegistry::INVALID_INDEX, "miss -> INVALID");

    PT_ASSERT(reg.pass(0).render_targets.size() == 2, "pass 0 has 2 targets");
    PT_ASSERT(reg.pass(1).pass_type == PassType::POST_PROCESS, "pass 1 type");

    PT_ASSERT(reg.passes().size() == 2, "passes() size");
}

void test_render_pass_attachment_ops_round_trip() {
    // RenderPassDef::attachment_ops is preserved by set_passes.
    AttachmentRegistry atts;
    auto defs = default_attachments();
    std::vector<AttachmentDef> vec(defs.begin(), defs.end());
    atts.set_defs(vec);

    RenderPassDef pd;
    pd.pass_name      = "Scene HDR";
    pd.render_targets = {"scene_hdr_color", "scene_depth"};
    pd.attachment_ops = default_attachment_ops(pd.render_targets, vec);

    RenderPassRegistry reg;
    std::vector<RenderPassDef> passes = {pd};
    reg.set_passes(passes);

    const RenderPassDef& p = reg.pass(0);
    PT_ASSERT(p.attachment_ops.size() == 2, "ops 2");
    PT_ASSERT(p.attachment_ops[0].attachment_name == "scene_hdr_color", "ops[0] name");
    PT_ASSERT(p.attachment_ops[0].load_op == AttachmentLoadOp::CLEAR, "ops[0] CLEAR");
    PT_ASSERT(p.attachment_ops[1].store_op == AttachmentStoreOp::DONT_CARE, "depth DONT_CARE");
}

} // anon

int main() {
    test_attachment_registry_index_resolution();
    test_attachment_registry_set_defs_replaces();
    test_render_pass_registry_index_resolution();
    test_render_pass_attachment_ops_round_trip();
    return pictor_test::report("unit_pipeline_registries_test");
}
