/// DecalSystem マテリアル — Vulkan device を要さない部分の検証.
///
/// `spec/feature/subsystem/decal.md` の「マテリアル」節。 pipeline 生成は
/// device が要るため見ない。 未登録ハンドルが組み込みへ寄ること、 記述子の
/// 既定値が従来の描画 (組み込みマテリアル・パラメータ 0) と同じことを見る。

#include "pictor/decal/decal_system.h"
#include "test_common.h"

using namespace pictor;
using namespace pictor_test;

int main() {
    // 組み込みだけが登録された状態 (registered = 1)。
    PT_ASSERT_OP(resolve_decal_material(DEFAULT_DECAL_MATERIAL, 1), ==, 0u,
                 "built-in handle resolves to slot 0");
    PT_ASSERT_OP(resolve_decal_material(1, 1), ==, 0u,
                 "unregistered handle falls back to built-in");

    // 独自マテリアルが 2 つ登録された状態 (registered = 3)。
    PT_ASSERT_OP(resolve_decal_material(2, 3), ==, 2u, "registered handle is kept");
    PT_ASSERT_OP(resolve_decal_material(3, 3), ==, 0u, "handle past the end falls back");
    PT_ASSERT_OP(resolve_decal_material(0xFFFFFFFFu, 3), ==, 0u, "garbage handle falls back");

    // 初期化前 (registered = 0) でも範囲外アクセスにならない値を返す。
    PT_ASSERT_OP(resolve_decal_material(5, 0), ==, 0u, "no materials yet");

    // 既定の記述子は従来どおり組み込みマテリアルで、 パラメータは全て 0。
    DecalDescriptor d;
    PT_ASSERT_OP(d.material, ==, DEFAULT_DECAL_MATERIAL, "default material is built-in");
    bool params_zero = true;
    for (float p : d.params) params_zero = params_zero && (p == 0.0f);
    PT_ASSERT(params_zero, "default params are zero");
    PT_ASSERT_OP(kDecalMaterialParamCount, ==, 8u, "params map to two vec4 in the shader");

    DecalSystem sys;
    PT_ASSERT_OP(sys.material_count(), ==, 0u, "no materials before initialize");

    return report("unit_decal_material_test");
}
