/// VisusDesc (v2) metadata -> ObjectDescriptor 共通フィールド。
/// handle 解決 / SceneRegistry 登録 / children 再帰は task 2 (VisusRuntime) で追加する。

#include "pictor/visus/visus.h"
#include "pictor/visus/visus_instantiator.h"
#include "test_common.h"

using namespace pictor;
using namespace pictor_test;

int main() {
    const float4x4 xf = float4x4::identity();
    const AABB     bb = {{0, 0, 0}, {1, 1, 1}};

    // 1. metadata 無しは v1 既定値 (DYNAMIC / layer 0 / lod 0)。
    {
        VisusDesc d;
        d.name = "empty_parts";
        d.kind = VisusKind::PRIMITIVE;

        const ObjectDescriptor base = visus_base_descriptor(d, xf, bb);
        PT_ASSERT_OP(base.flags, ==, ObjectFlags::set_layer(ObjectFlags::DYNAMIC, 0), "default flags");
        PT_ASSERT_OP(base.lodLevel, ==, 0, "default lod");
        PT_ASSERT_OP(base.shaderKey, ==, uint64_t{0}, "default shaderKey");

    }

    // 2. metadata の render.* が反映される。
    {
        VisusDesc d;
        d.name = "multi_part";
        d.kind = VisusKind::MODEL;
        d.metadata.set(visus_keys::kRenderFlags, static_cast<int>(ObjectFlags::STATIC));
        d.metadata.set(visus_keys::kRenderLayer, 2);
        d.metadata.set(visus_keys::kRenderLod, 3);
        d.metadata.set(visus_keys::kShaderKeyOverride, 77);

        const ObjectDescriptor base = visus_base_descriptor(d, xf, bb);
        PT_ASSERT_OP(base.flags, ==, ObjectFlags::set_layer(ObjectFlags::STATIC, 2), "render.flags/layer applied");
        PT_ASSERT_OP(base.lodLevel, ==, 3, "render.lod applied");
        PT_ASSERT_OP(base.shaderKey, ==, uint64_t{77}, "shader.key_override applied");

    }

    // 3. 不正な metadata (負数 / 小数 / 範囲外) は既定値へ落ちる。
    {
        VisusDesc d;
        d.name = "bad_meta";
        d.metadata.set(visus_keys::kRenderFlags, 2.5);
        d.metadata.set(visus_keys::kRenderLod, -5);
        d.metadata.set(visus_keys::kRenderLayer, 4);
        d.metadata.set(visus_keys::kShaderKeyOverride, 1e300);
        const ObjectDescriptor base = visus_base_descriptor(d, xf, bb);
        PT_ASSERT_OP(base.flags, ==, ObjectFlags::set_layer(ObjectFlags::DYNAMIC, 0), "fractional flags -> default");
        PT_ASSERT_OP(base.lodLevel, ==, 0, "negative lod -> default");
        PT_ASSERT_OP(base.shaderKey, ==, uint64_t{0}, "out-of-range shader key -> default");
    }

    // 4. shader.key_override は予約済み custom-shader ビットを設定できない。
    {
        VisusDesc d;
        d.name = "reserved_shader_bits";
        d.metadata.set(visus_keys::kShaderKeyOverride,
                       static_cast<double>(ShaderKey::CUSTOM_FLAG));
        const ObjectDescriptor base = visus_base_descriptor(d, xf, bb);
        PT_ASSERT_OP(base.shaderKey, ==, uint64_t{0}, "reserved shader bits -> default");
        PT_ASSERT(!ShaderKey::is_custom(base.shaderKey),
                  "metadata cannot synthesize a custom shader handle");
    }

    return report("unit_visus_instantiator_test");
}
