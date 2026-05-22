// unit_attachment_def_test — Phase 3 schema v2 の基礎確認。
//
// 1. AttachmentDef / AttachmentOpsDef の to_string / parse_* が round-trip
// 2. default_attachments() が 3 件 (scene_hdr_color / scene_depth / swapchain)
//    を期待順で返す
// 3. default_attachment_ops() が kind に応じて load/store/layout を選ぶ
// 4. PipelineProfileDef に v2 構造 (attachments + render_passes.attachment_ops)
//    を入れて to_pipeline_profile_json -> from_pipeline_profile_json が
//    バイト単位ではないが意味単位で同値 (round-trip)

#include "pictor/pipeline/attachment_def.h"
#include "pictor/pipeline/pipeline_profile.h"
#include "pictor/pipeline/pipeline_profile_serializer.h"
#include "test_common.h"

#include <string>

using namespace pictor;
using pictor_test::g_failures;

namespace {

void test_enum_round_trip() {
    AttachmentKind k;
    PT_ASSERT(parse_attachment_kind("COLOR", k) && k == AttachmentKind::COLOR, "COLOR");
    PT_ASSERT(parse_attachment_kind("DEPTH", k) && k == AttachmentKind::DEPTH, "DEPTH");
    PT_ASSERT(parse_attachment_kind("SWAPCHAIN_COLOR", k) && k == AttachmentKind::SWAPCHAIN_COLOR, "SWAPCHAIN");
    PT_ASSERT(!parse_attachment_kind("BOGUS", k), "rejects unknown");

    AttachmentFormat f;
    PT_ASSERT(parse_attachment_format("R16G16B16A16_SFLOAT", f)
              && f == AttachmentFormat::R16G16B16A16_SFLOAT, "HDR");
    PT_ASSERT(parse_attachment_format("D32_SFLOAT", f)
              && f == AttachmentFormat::D32_SFLOAT, "D32");

    AttachmentLoadOp lo;
    PT_ASSERT(parse_attachment_load_op("CLEAR", lo) && lo == AttachmentLoadOp::CLEAR, "load CLEAR");
    AttachmentStoreOp so;
    PT_ASSERT(parse_attachment_store_op("STORE", so) && so == AttachmentStoreOp::STORE, "store STORE");

    ImageLayout l;
    PT_ASSERT(parse_image_layout("PRESENT_SRC_KHR", l) && l == ImageLayout::PRESENT_SRC_KHR, "PRESENT");
    PT_ASSERT(parse_image_layout("UNDEFINED", l) && l == ImageLayout::UNDEFINED, "UNDEFINED");

    PT_ASSERT(std::string(to_string(AttachmentFormat::B8G8R8A8_SRGB)) == "B8G8R8A8_SRGB", "BGRA SRGB");
}

void test_is_depth_format() {
    PT_ASSERT(is_depth_format(AttachmentFormat::D32_SFLOAT), "D32 is depth");
    PT_ASSERT(is_depth_format(AttachmentFormat::D24_UNORM_S8_UINT), "D24S8 is depth");
    PT_ASSERT(!is_depth_format(AttachmentFormat::R16G16B16A16_SFLOAT), "HDR not depth");
    PT_ASSERT(!is_depth_format(AttachmentFormat::UNDEFINED), "UNDEFINED not depth");
}

void test_default_attachments() {
    auto defs = default_attachments();
    PT_ASSERT(defs.size() == 3, "3 defaults");
    PT_ASSERT(defs[0].name == "scene_hdr_color", "[0] hdr");
    PT_ASSERT(defs[0].kind == AttachmentKind::COLOR, "[0] COLOR kind");
    PT_ASSERT(defs[0].format == AttachmentFormat::R16G16B16A16_SFLOAT, "[0] HDR fmt");
    PT_ASSERT(defs[1].name == "scene_depth", "[1] depth");
    PT_ASSERT(defs[1].kind == AttachmentKind::DEPTH, "[1] DEPTH kind");
    PT_ASSERT(defs[2].name == "swapchain", "[2] swapchain");
    PT_ASSERT(defs[2].kind == AttachmentKind::SWAPCHAIN_COLOR, "[2] SWAPCHAIN kind");
}

void test_default_attachment_ops() {
    auto defs = default_attachments();
    std::vector<AttachmentDef> vec(defs.begin(), defs.end());

    // Color target -> CLEAR / STORE / SHADER_READ_ONLY_OPTIMAL
    auto ops_color = default_attachment_ops({"scene_hdr_color"}, vec);
    PT_ASSERT(ops_color.size() == 1, "color: 1 entry");
    PT_ASSERT(ops_color[0].load_op  == AttachmentLoadOp::CLEAR, "color CLEAR");
    PT_ASSERT(ops_color[0].store_op == AttachmentStoreOp::STORE, "color STORE");
    PT_ASSERT(ops_color[0].final_layout == ImageLayout::SHADER_READ_ONLY_OPTIMAL, "color SHADER_READ");

    // Depth target -> CLEAR / DONT_CARE / DEPTH_STENCIL_ATTACHMENT_OPTIMAL
    auto ops_depth = default_attachment_ops({"scene_depth"}, vec);
    PT_ASSERT(ops_depth.size() == 1, "depth: 1 entry");
    PT_ASSERT(ops_depth[0].load_op  == AttachmentLoadOp::CLEAR, "depth CLEAR");
    PT_ASSERT(ops_depth[0].store_op == AttachmentStoreOp::DONT_CARE, "depth DONT_CARE");
    PT_ASSERT(ops_depth[0].final_layout == ImageLayout::DEPTH_STENCIL_ATTACHMENT_OPTIMAL, "depth DSV");

    // Swapchain -> CLEAR / STORE / PRESENT_SRC_KHR
    auto ops_swap = default_attachment_ops({"swapchain"}, vec);
    PT_ASSERT(ops_swap.size() == 1, "swap: 1 entry");
    PT_ASSERT(ops_swap[0].load_op  == AttachmentLoadOp::CLEAR, "swap CLEAR");
    PT_ASSERT(ops_swap[0].store_op == AttachmentStoreOp::STORE, "swap STORE");
    PT_ASSERT(ops_swap[0].final_layout == ImageLayout::PRESENT_SRC_KHR, "swap PRESENT");
}

void test_profile_v2_round_trip() {
    PipelineProfileDef in;
    in.profile_name = "Phase3Test";
    {
        auto defs = default_attachments();
        in.attachments.assign(defs.begin(), defs.end());
    }
    RenderPassDef rp;
    rp.pass_name      = "Scene HDR";
    rp.pass_type      = PassType::OPAQUE;
    rp.render_targets = {"scene_hdr_color", "scene_depth"};
    rp.attachment_ops = default_attachment_ops(rp.render_targets, in.attachments);
    in.render_passes.push_back(rp);

    std::string json = to_pipeline_profile_json(in);
    PT_ASSERT(json.find("\"version\": 2") != std::string::npos, "emits v2");
    PT_ASSERT(json.find("\"attachments\"") != std::string::npos, "emits attachments key");
    PT_ASSERT(json.find("\"scene_hdr_color\"") != std::string::npos, "hdr name present");
    PT_ASSERT(json.find("\"attachment_ops\"") != std::string::npos, "emits attachment_ops");

    PipelineProfileDef out;
    std::string err;
    bool ok = from_pipeline_profile_json(json, out, &err);
    PT_ASSERT(ok, err.c_str());
    PT_ASSERT(out.profile_name == "Phase3Test", "name kept");
    PT_ASSERT(out.attachments.size() == 3, "3 attachments parsed");
    PT_ASSERT(out.attachments[0].name == "scene_hdr_color", "name[0]");
    PT_ASSERT(out.attachments[0].format == AttachmentFormat::R16G16B16A16_SFLOAT, "fmt[0]");
    PT_ASSERT(out.attachments[1].kind == AttachmentKind::DEPTH, "kind[1]");
    PT_ASSERT(out.attachments[2].kind == AttachmentKind::SWAPCHAIN_COLOR, "kind[2]");

    PT_ASSERT(out.render_passes.size() == 1, "1 pass");
    PT_ASSERT(out.render_passes[0].attachment_ops.size() == 2, "2 ops");
    PT_ASSERT(out.render_passes[0].attachment_ops[0].attachment_name == "scene_hdr_color", "ops[0] att");
    PT_ASSERT(out.render_passes[0].attachment_ops[0].load_op == AttachmentLoadOp::CLEAR, "ops[0] CLEAR");
    PT_ASSERT(out.render_passes[0].attachment_ops[1].store_op == AttachmentStoreOp::DONT_CARE,
              "depth STORE DONT_CARE");
}

void test_v1_backward_compat() {
    // v1 profile (no attachments[], no attachment_ops[]) — must parse with
    // empty attachments / empty attachment_ops.
    const char* json =
        "{\n"
        "  \"version\": 1,\n"
        "  \"profile_name\": \"Legacy\",\n"
        "  \"render_passes\": [ { \"pass_name\": \"Opaque\",\n"
        "                          \"pass_type\": \"OPAQUE\",\n"
        "                          \"render_targets\": [\"scene_hdr_color\"]\n"
        "                        } ]\n"
        "}";
    PipelineProfileDef out;
    std::string err;
    bool ok = from_pipeline_profile_json(json, out, &err);
    PT_ASSERT(ok, err.c_str());
    PT_ASSERT(out.profile_name == "Legacy", "v1 name");
    PT_ASSERT(out.attachments.empty(), "v1 attachments empty");
    PT_ASSERT(out.render_passes.size() == 1, "1 pass");
    PT_ASSERT(out.render_passes[0].attachment_ops.empty(), "v1 ops empty");
}

} // anon

int main() {
    test_enum_round_trip();
    test_is_depth_format();
    test_default_attachments();
    test_default_attachment_ops();
    test_profile_v2_round_trip();
    test_v1_backward_compat();
    return pictor_test::report("unit_attachment_def_test");
}
