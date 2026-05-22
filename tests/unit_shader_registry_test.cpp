/// ShaderRegistry — カスタムシェーダの登録 / handle 管理 (ヘッドレス).
///
/// VkPipeline 生成 (build_pipelines) は Vulkan device を要するため
/// ここでは検証しない。 登録ロジックと handle ↔ 定義の対応のみを見る。

#include "pictor/shader/shader_registry.h"
#include "test_common.h"

using namespace pictor;
using namespace pictor_test;

int main() {
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
