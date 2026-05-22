#include "pictor/visus/visus_instantiator.h"

namespace pictor {

std::vector<ObjectId> instantiate_visus(
    SceneRegistry&    scene,
    const VisusDesc&  desc,
    const float4x4&   transform,
    const AABB&       bounds)
{
    // 共通部分を 1 度組み立てて、 slot ごとに material/materialKey だけ差替える。
    ObjectDescriptor base{};
    base.mesh        = desc.mesh;
    base.transform   = transform;
    base.bounds      = bounds;
    base.flags       = ObjectFlags::set_layer(desc.default_flags, desc.layer);
    base.shaderKey   = desc.shader_key_override;
    base.lodLevel    = desc.initial_lod;

    // CUSTOM kind: `desc.shader` (ShaderHandle) を ObjectDescriptor に伝播する。
    // 従来は捨てられていた (`spec/rendering-extensibility-design.md` §2)。
    // - `customShader` フィールドに直接設定 (レンダラが参照する正)。
    // - `shaderKey` 上位ビットにも畳み込み、 SoA stream を増やさずに
    //   バッチ整列でカスタムシェーダ object をグループ化する。
    // 固定 PBR マテリアル層は温存し、 customShader が有効な object のみ
    // PBR 経路をバイパスする。
    if (desc.geometry_kind == VisusGeometryKind::CUSTOM &&
        desc.shader != INVALID_SHADER) {
        base.customShader = desc.shader;
        base.shaderKey    = ShaderKey::with_custom_shader(base.shaderKey, desc.shader);
    }

    std::vector<ObjectId> ids;
    const size_t slot_count = desc.materials.empty() ? 1 : desc.materials.size();
    ids.reserve(slot_count);

    for (size_t i = 0; i < slot_count; ++i) {
        ObjectDescriptor od = base;
        if (!desc.materials.empty()) {
            od.material    = desc.materials[i].material;
            od.materialKey = static_cast<uint32_t>(desc.materials[i].material);
        }
        ids.push_back(scene.register_object(od));
    }
    return ids;
}

} // namespace pictor
