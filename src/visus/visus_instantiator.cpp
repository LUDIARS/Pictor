#include "pictor/visus/visus_instantiator.h"

#include <cmath>
#include <limits>
#include <string>

namespace pictor {

namespace {

uint64_t number_or(const VisusMetadata& m, const char* key,
                   uint64_t def, uint64_t max) {
    const auto v = m.get_number(key);
    if (!v || !std::isfinite(*v) || *v < 0.0 ||
        *v > static_cast<double>(max) ||
        std::trunc(*v) != *v) return def;
    return static_cast<uint64_t>(*v);
}

} // namespace

ObjectDescriptor visus_base_descriptor(const VisusDesc& desc,
                                       const float4x4&  transform,
                                       const AABB&      bounds)
{
    ObjectDescriptor base{};
    base.transform = transform;
    base.bounds    = bounds;

    const uint16_t flags = static_cast<uint16_t>(
        number_or(desc.metadata, visus_keys::kRenderFlags, ObjectFlags::DYNAMIC,
                  std::numeric_limits<uint16_t>::max()));
    const uint16_t layer = static_cast<uint16_t>(
        number_or(desc.metadata, visus_keys::kRenderLayer, 0, 3));
    base.flags     = ObjectFlags::set_layer(flags, layer);
    base.lodLevel  = static_cast<uint8_t>(
        number_or(desc.metadata, visus_keys::kRenderLod, 0,
                  std::numeric_limits<uint8_t>::max()));
    // ShaderKey reserves the upper 32 bits for the custom-shader flag and
    // handle. Metadata may only supply the lower variant bits; otherwise an
    // untrusted Visus file could synthesize a resolved custom shader.
    base.shaderKey = number_or(desc.metadata, visus_keys::kShaderKeyOverride, 0,
                               std::numeric_limits<uint32_t>::max());
    return base;
}

// ---- VisusInstance -----------------------------------------------------------

void VisusInstance::collect_objects(std::vector<ObjectId>& out) const {
    out.insert(out.end(), objects.begin(), objects.end());
    for (const VisusInstance& c : children) c.collect_objects(out);
}

// ---- transform ----------------------------------------------------------------

float4x4 visus_compose(const float4x4& local, const float4x4& parent) {
    float4x4 r{};
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) {
            float acc = 0.0f;
            for (int k = 0; k < 4; ++k) acc += local.m[i][k] * parent.m[k][j];
            r.m[i][j] = acc;
        }
    }
    return r;
}

namespace {

void warn(std::vector<std::string>* w, std::string msg) {
    if (w) w->push_back(std::move(msg));
}

// 子 instance の transform: attach.bone が無ければ offset × parent。
// bone 指定時は解決できた場合だけ offset × bone × parent とし、解決不能なら
// offset も適用せず parent をそのまま使う (§2.4 の安全な縮退契約)。
float4x4 child_transform(const VisusDesc& parent_desc, const VisusResolved* parent_res,
                         const VisusChildRef& ref, const float4x4& parent_xf,
                         IVisusResolver* resolver, std::vector<std::string>* warnings) {
    float4x4 local = float4x4::identity();
    local.set_translation(ref.attach.offset[0], ref.attach.offset[1], ref.attach.offset[2]);

    if (!ref.attach.has_bone()) return visus_compose(local, parent_xf);

    float4x4 bone = float4x4::identity();
    bool have_bone = false;
    if (parent_desc.kind != VisusKind::MODEL) {
        warn(warnings, parent_desc.name + " -> " + ref.visus +
                       ": attach.bone on non-model parent ignored");
    } else if (!resolver || !parent_res || parent_res->model == INVALID_MODEL) {
        warn(warnings, parent_desc.name + " -> " + ref.visus +
                       ": bone '" + ref.attach.bone + "' unavailable (no resolver / model)");
    } else if (!resolver->bone_transform(parent_res->model, ref.attach.bone, bone)) {
        warn(warnings, parent_desc.name + " -> " + ref.visus +
                       ": bone '" + ref.attach.bone + "' not found");
    } else {
        have_bone = true;
    }
    return have_bone ? visus_compose(visus_compose(local, bone), parent_xf)
                     : parent_xf;
}

// この Visus 自身の ObjectDescriptor を登録する (children は含まない)。
void register_objects(SceneRegistry& scene, const VisusDesc& desc, const VisusResolved* res,
                      const float4x4& xf, const AABB& bounds, VisusInstance& inst) {
    const ObjectDescriptor base = visus_base_descriptor(desc, xf, bounds);

    switch (desc.kind) {
        case VisusKind::MODEL: {
            if (!res) break;
            for (size_t i = 0; i < res->parts.size(); ++i) {
                const VisusResolvedPart& rp = res->parts[i];
                if (rp.mesh == INVALID_MESH) continue;
                ObjectDescriptor od = base;
                od.materialKey = static_cast<uint32_t>(i);
                od.mesh         = rp.mesh;
                od.customShader = rp.shader;
                od.material     = rp.material;
                // shader_key は key_override (part 優先) + custom ビット込み。
                od.shaderKey    = rp.shader_key;
                if (rp.material != INVALID_MATERIAL)
                    od.materialKey = static_cast<uint32_t>(rp.material);
                inst.objects.push_back(scene.register_object(od));
            }
            break;
        }
        case VisusKind::PRIMITIVE: {
            if (!res || res->mesh == INVALID_MESH) break;
            ObjectDescriptor od = base;
            od.mesh         = res->mesh;
            od.customShader = res->shader;
            od.material     = res->material;
            // 解決済み shader があるときだけ side-table の key を採る。 未解決の
            // ままだと shader_key は base と同じ key_override なので、 0 で
            // 上書きして metadata の指定を落とさない。
            if (res->shader != INVALID_SHADER) od.shaderKey = res->shader_key;
            if (res->material != INVALID_MATERIAL)
                od.materialKey = static_cast<uint32_t>(res->material);
            inst.objects.push_back(scene.register_object(od));
            break;
        }
        case VisusKind::CUSTOM:
            break;
        case VisusKind::RIVE:
        case VisusKind::UI:
        case VisusKind::PARTICLE:
        case VisusKind::TEXT:
            if (res) inst.generic_handle = res->generic_handle;
            break;
        case VisusKind::GROUP:
        case VisusKind::NONE:
            break;
    }
}

void instantiate_rec(SceneRegistry& scene, const VisusCatalog& catalog,
                     const VisusRuntime& runtime, const VisusDesc& desc,
                     const float4x4& xf, const AABB& bounds, int depth,
                     std::vector<std::string>& stack, VisusInstance& out,
                     std::vector<std::string>* warnings, IVisusResolver* resolver) {
    out.name      = desc.name;
    out.transform = xf;
    const VisusResolved* res = runtime.get(desc.name);
    register_objects(scene, desc, res, xf, bounds, out);

    stack.push_back(desc.name);
    for (const VisusChildRef& c : desc.children) {
        std::string err;
        const VisusDesc* child = catalog.resolve_child(desc, c, &err);
        if (!child) {
            warn(warnings, desc.name + ": " + err);
            continue;
        }
        bool cyclic = false;
        for (const std::string& s : stack) if (s == child->name) { cyclic = true; break; }
        if (cyclic) {
            warn(warnings, "visus cycle at: " + child->name);
            continue;
        }
        if (depth + 1 > VisusCatalog::kMaxChildDepth) {
            warn(warnings, "visus nesting too deep at: " + child->name);
            continue;
        }
        VisusInstance ci;
        const float4x4 cxf = child_transform(desc, res, c, xf, resolver, warnings);
        instantiate_rec(scene, catalog, runtime, *child, cxf, bounds, depth + 1,
                        stack, ci, warnings, resolver);
        out.children.push_back(std::move(ci));
    }
    stack.pop_back();
}

} // namespace

bool instantiate_visus(SceneRegistry&            scene,
                       const VisusCatalog&       catalog,
                       const VisusRuntime&       runtime,
                       std::string_view          name,
                       const float4x4&           transform,
                       const AABB&               bounds,
                       VisusInstance&            out,
                       std::vector<std::string>* warnings,
                       IVisusResolver*           resolver) {
    const VisusDesc* desc = catalog.find(name);
    if (!desc) {
        warn(warnings, "visus not found: " + std::string(name));
        return false;
    }
    out = VisusInstance{};
    std::vector<std::string> stack;
    instantiate_rec(scene, catalog, runtime, *desc, transform, bounds, 0, stack,
                    out, warnings, resolver);
    return true;
}

} // namespace pictor
