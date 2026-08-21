/// ShaderRegistry — カスタムシェーダの登録 / handle 管理 (ヘッドレス).
///
/// VkPipeline 生成 (build_pipelines) は Vulkan device を要するため
/// ここでは検証しない。 登録ロジックと handle ↔ 定義の対応のみを見る。

#include "pictor/shader/shader_registry.h"
#include "test_common.h"

using namespace pictor;
using namespace pictor_test;

int main() {
    // Headless builds expose the same hot-reload seam as Vulkan hosts. It is a
    // no-op so host wiring does not need backend-specific preprocessor guards.
#ifndef PICTOR_HAS_VULKAN
    {
        ShaderRegistry reg;
        PT_ASSERT(reg.rebuild_pipelines(), "headless rebuild is a successful no-op");
    }
#endif

    // 0b. build target の無効化はどの backend でも安全に呼べる。 Vulkan 有効時
    //     のみ実体があり、 未 build の registry でも副作用なく呼べること
    //     (ホストは render pass 破棄前に無条件で呼ぶ)。
#ifdef PICTOR_HAS_VULKAN
    {
        ShaderRegistry reg;
        reg.invalidate_build_target();
        PT_ASSERT(!reg.is_built(), "invalidate on a never-built registry stays unbuilt");
        // build target を失った registry は未 build 扱いなので、 rebuild は
        // dangling handle に触れず no-op success で戻る。 ホストは新しい
        // render pass で build_pipelines() を呼び直す。
        PT_ASSERT(reg.rebuild_pipelines(),
                  "unbuilt registry rebuild is still a no-op success");
    }
#endif

    // 1. vert+frag が揃った定義は登録でき、 handle が 0 始まりの連番。
    {
        ShaderRegistry reg;
        CustomShaderDef a;
        a.name     = "toon";
        a.vert_spv = "shaders/toon.vert.spv";
        a.frag_spv = "shaders/toon.frag.spv";

        CustomShaderDef b;
        b.name     = "dissolve";
        b.vert_spv = "shaders/dissolve.vert.spv";
        b.frag_spv = "shaders/dissolve.frag.spv";

        ShaderHandle ha = reg.register_shader(a);
        ShaderHandle hb = reg.register_shader(b);
        PT_ASSERT_OP(ha, ==, ShaderHandle{0}, "first handle is 0");
        PT_ASSERT_OP(hb, ==, ShaderHandle{1}, "second handle is 1");
        PT_ASSERT_OP(reg.count(), ==, size_t{2}, "registry size matches");

        const CustomShaderDef* got = reg.get(ha);
        PT_ASSERT(got != nullptr, "get returns non-null");
        PT_ASSERT(got->name == "toon", "get returns the right entry");

        PT_ASSERT(reg.get(99) == nullptr, "out-of-range handle -> nullptr");
        PT_ASSERT(reg.get(INVALID_SHADER) == nullptr,
                  "INVALID_SHADER -> nullptr");
    }

    // 1b. §6.2: CustomShaderDef::vertex_layout が登録後も保持される。
    {
        ShaderRegistry reg;
        CustomShaderDef def;
        def.name     = "mesh_driven";
        def.vert_spv = "shaders/mesh.vert.spv";
        def.frag_spv = "shaders/mesh.frag.spv";
        def.vertex_layout.stride = 64;
        def.vertex_layout.attributes = {
            {VertexSemantic::POSITION,  VertexAttributeType::FLOAT3,   0},
            {VertexSemantic::TEXCOORD0, VertexAttributeType::FLOAT2,   12},
            {VertexSemantic::JOINTS,    VertexAttributeType::UINT32X4, 20},
        };
        ShaderHandle h = reg.register_shader(def);
        PT_ASSERT_OP(h, !=, INVALID_SHADER, "vert+frag def with layout accepted");

        const CustomShaderDef* got = reg.get(h);
        PT_ASSERT(got != nullptr, "registered def retrievable");
        PT_ASSERT(!got->vertex_layout.empty(), "vertex_layout preserved (non-empty)");
        PT_ASSERT_OP(got->vertex_layout.attributes.size(), ==, size_t{3},
                     "all 3 attributes preserved");
        PT_ASSERT_OP(got->vertex_layout.computed_stride(), ==, uint32_t{64},
                     "explicit stride preserved");

        // 空 vertex_layout の def は phase 1 互換フォールバック。
        CustomShaderDef legacy;
        legacy.name     = "index_driven";
        legacy.vert_spv = "shaders/idx.vert.spv";
        legacy.frag_spv = "shaders/idx.frag.spv";
        ShaderHandle hl = reg.register_shader(legacy);
        PT_ASSERT_OP(hl, !=, INVALID_SHADER, "layout-less def still accepted");
        PT_ASSERT(reg.get(hl)->vertex_layout.empty(),
                  "absent vertex_layout stays empty (phase 1 fallback)");
    }

    // 2. vert / frag が欠けた定義は拒否される (phase 1 は固定 vert+frag のみ)。
    {
        ShaderRegistry reg;
        CustomShaderDef no_frag;
        no_frag.name     = "broken";
        no_frag.vert_spv = "shaders/x.vert.spv";
        // frag_spv は空

        ShaderHandle h = reg.register_shader(no_frag);
        PT_ASSERT_OP(h, ==, INVALID_SHADER, "missing frag -> INVALID_SHADER");
        PT_ASSERT_OP(reg.count(), ==, size_t{0}, "rejected def is not stored");

        CustomShaderDef no_vert;
        no_vert.name     = "broken2";
        no_vert.frag_spv = "shaders/x.frag.spv";
        PT_ASSERT_OP(reg.register_shader(no_vert), ==, INVALID_SHADER,
                     "missing vert -> INVALID_SHADER");
    }

    return report("unit_shader_registry_test");
}
