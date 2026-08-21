/// instantiate_visus (v2) — parts → ObjectDescriptor 群 / children の transform 合成 /
/// group / 循環・未解決の扱い。 handle は VisusRuntime の解決結果から。

#include "pictor/memory/memory_subsystem.h"
#include "pictor/batch/batch_builder.h"
#include "pictor/memory/frame_allocator.h"
#include "pictor/scene/scene_registry.h"
#include "pictor/visus/visus.h"
#include "pictor/visus/visus_catalog.h"
#include "pictor/visus/visus_instantiator.h"
#include "pictor/visus/visus_runtime.h"
#include "test_common.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

using namespace pictor;
using namespace pictor_test;

namespace {

bool has(const std::vector<std::string>& v, const char* needle) {
    return std::any_of(v.begin(), v.end(), [&](const std::string& s) { return s.find(needle) != std::string::npos; });
}

bool near(float a, float b) { return std::fabs(a - b) < 1e-5f; }

bool has_batch_mesh(const std::vector<RenderBatch>& batches, MeshHandle mesh) {
    return std::any_of(batches.begin(), batches.end(),
                       [mesh](const RenderBatch& batch) { return batch.mesh == mesh; });
}

float4x4 translation(float x, float y, float z) {
    float4x4 m = float4x4::identity();
    m.set_translation(x, y, z);
    return m;
}

// bone_transform だけ実装する resolver (instantiate は bone 行列にしか使わない)。
struct BoneResolver : IVisusResolver {
    bool bone_transform(ModelHandle, std::string_view bone, float4x4& out) override {
        if (bone != "Head") return false;
        out = translation(0.0f, 1.5f, 0.0f);
        return true;
    }
};

VisusResolved resolved_model() {
    VisusResolved r;
    r.name  = "kuzuha";
    r.kind  = VisusKind::MODEL;
    r.model = 7;
    VisusResolvedPart a; a.part = "cloak"; a.mesh = 31; a.shader = INVALID_SHADER; a.shader_key = 9;
    VisusResolvedPart b; b.part = "face";  b.mesh = 32; b.shader = 3; b.shader_key = ShaderKey::with_custom_shader(5, 3); b.material = 11;
    r.parts = {a, b};
    return r;
}

void test_base_descriptor_defaults_and_metadata() {
    const float4x4 xf = float4x4::identity();
    const AABB     bb = {{0, 0, 0}, {1, 1, 1}};

    VisusDesc d;
    d.name = "plain";
    ObjectDescriptor base = visus_base_descriptor(d, xf, bb);
    PT_ASSERT_OP(base.flags, ==, ObjectFlags::set_layer(ObjectFlags::DYNAMIC, 0), "default flags");
    PT_ASSERT_OP(base.lodLevel, ==, 0, "default lod");
    PT_ASSERT_OP(base.shaderKey, ==, uint64_t{0}, "default shaderKey");

    d.metadata.set(visus_keys::kRenderFlags, static_cast<int>(ObjectFlags::STATIC));
    d.metadata.set(visus_keys::kRenderLayer, 2);
    d.metadata.set(visus_keys::kRenderLod, 3);
    d.metadata.set(visus_keys::kShaderKeyOverride, 77);
    base = visus_base_descriptor(d, xf, bb);
    PT_ASSERT_OP(base.flags, ==, ObjectFlags::set_layer(ObjectFlags::STATIC, 2), "render.flags/layer applied");
    PT_ASSERT_OP(base.lodLevel, ==, 3, "render.lod applied");
    PT_ASSERT_OP(base.shaderKey, ==, uint64_t{77}, "shader.key_override applied");

    d.metadata.set(visus_keys::kRenderFlags, "not a number");
    d.metadata.set(visus_keys::kRenderLod, -5);
    d.metadata.set(visus_keys::kShaderKeyOverride, 1e300);
    base = visus_base_descriptor(d, xf, bb);
    PT_ASSERT_OP(base.flags, ==, ObjectFlags::set_layer(ObjectFlags::DYNAMIC, 2), "string flags -> default");
    PT_ASSERT_OP(base.lodLevel, ==, 0, "negative lod -> default");
    PT_ASSERT_OP(base.shaderKey, ==, uint64_t{0}, "out-of-range shader key -> default");

    // shader.key_override は予約済み custom-shader ビットを設定できない。
    {
        VisusDesc reserved;
        reserved.name = "reserved_shader_bits";
        reserved.metadata.set(visus_keys::kShaderKeyOverride,
                              static_cast<double>(ShaderKey::CUSTOM_FLAG));
        const ObjectDescriptor rb = visus_base_descriptor(reserved, xf, bb);
        PT_ASSERT_OP(rb.shaderKey, ==, uint64_t{0}, "reserved shader bits -> default");
        PT_ASSERT(!ShaderKey::is_custom(rb.shaderKey),
                  "metadata cannot synthesize a custom shader handle");
    }
}

void test_compose() {
    const float4x4 a = translation(1, 2, 3);
    const float4x4 b = translation(10, 20, 30);
    const float4x4 c = visus_compose(a, b);
    const float3 t = c.get_translation();
    PT_ASSERT(near(t.x, 11) && near(t.y, 22) && near(t.z, 33), "translation composes additively");
    const float4x4 i = visus_compose(float4x4::identity(), b);
    PT_ASSERT(near(i.get_translation().y, 20), "identity × b = b");

    float4x4 parent_scale = float4x4::identity();
    parent_scale.m[0][0] = 2.0f;
    parent_scale.m[1][1] = 3.0f;
    parent_scale.m[2][2] = 4.0f;
    const float3 scaled = visus_compose(translation(1, 2, 3), parent_scale).get_translation();
    PT_ASSERT(near(scaled.x, 2) && near(scaled.y, 6) && near(scaled.z, 12),
              "row-vector local transform is applied before parent transform");
}

void test_model_parts_and_children() {
    MemorySubsystem memory;
    SceneRegistry   scene(memory);

    VisusCatalog cat;
    VisusDesc kuzuha; kuzuha.name = "kuzuha"; kuzuha.kind = VisusKind::MODEL; kuzuha.asset = "k.fbx";
    VisusPart p0; p0.part = "cloak"; VisusPart p1; p1.part = "face";
    kuzuha.parts = {p0, p1};
    kuzuha.metadata.set(visus_keys::kRenderLayer, 1);
    VisusChildRef facial; facial.visus = "kuzuha_facial"; facial.attach.bone = "Head"; facial.attach.offset[2] = 0.05f;
    VisusChildRef aura;   aura.visus   = "aura";                                  // bone 無し → 親 transform
    VisusChildRef ghost;  ghost.visus  = "missing";                               // 未解決
    kuzuha.children = {facial, aura, ghost};
    cat.add(kuzuha);
    VisusDesc f; f.name = "kuzuha_facial"; f.kind = VisusKind::RIVE; f.asset = "face.riv"; cat.add(f);
    VisusDesc g; g.name = "aura"; g.kind = VisusKind::GROUP;
    VisusChildRef cube; cube.visus = "cube"; cube.attach.offset[0] = 2.0f; g.children.push_back(cube); cat.add(g);
    VisusDesc c; c.name = "cube"; c.kind = VisusKind::PRIMITIVE; c.asset = "cube.mesh"; cat.add(c);

    VisusRuntime rt;
    rt.set(resolved_model());
    VisusResolved rf; rf.name = "kuzuha_facial"; rf.kind = VisusKind::RIVE;
    rf.generic_handle = 51; rt.set(rf);
    VisusResolved rc; rc.name = "cube"; rc.kind = VisusKind::PRIMITIVE; rc.mesh = 21; rt.set(rc);

    BoneResolver bones;
    VisusInstance inst;
    std::vector<std::string> w;
    const AABB bb = {{-1, -1, -1}, {1, 1, 1}};
    PT_ASSERT(instantiate_visus(scene, cat, rt, "kuzuha", translation(100, 0, 0), bb, inst, &w, &bones),
              "root instantiates");

    // 自身: part ごとに 1 個
    PT_ASSERT(inst.name == "kuzuha", "instance name");
    PT_ASSERT_OP(inst.objects.size(), ==, size_t{2}, "2 parts -> 2 objects");
    PT_ASSERT(inst.objects[0] != inst.objects[1], "distinct ids");
    for (size_t i = 0; i < inst.objects.size(); ++i) {
        const SceneRegistry::ObjectLocation loc = scene.find_object(inst.objects[i]);
        PT_ASSERT(loc.valid, "objects registered");
        PT_ASSERT_OP(scene.pool(loc.pool_type).mesh_handles()[loc.pool_index], ==,
                     static_cast<MeshHandle>(31 + i), "model object carries drawable part mesh");
    }

    // children: facial (bone) / aura (group → 0 objects, 1 grandchild) / missing (skipped + warning)
    PT_ASSERT_OP(inst.children.size(), ==, size_t{2}, "2 resolvable children");
    const VisusInstance& fi = inst.children[0];
    PT_ASSERT(fi.name == "kuzuha_facial" && fi.objects.empty() && fi.generic_handle == 51,
              "rive child exposes host generic handle without invalid mesh object");
    {
        const float3 t = fi.transform.get_translation();
        PT_ASSERT(near(t.x, 100) && near(t.y, 1.5f) && near(t.z, 0.05f),
                  "child transform = offset × bone × parent");
    }
    const VisusInstance& ai = inst.children[1];
    PT_ASSERT(ai.name == "aura" && ai.objects.empty(), "group has no objects");
    PT_ASSERT(near(ai.transform.get_translation().x, 100), "no-bone child inherits parent transform");
    PT_ASSERT_OP(ai.children.size(), ==, size_t{1}, "group has 1 grandchild");
    PT_ASSERT(ai.children[0].name == "cube" && ai.children[0].objects.size() == 1, "primitive grandchild 1 object");
    PT_ASSERT(near(ai.children[0].transform.get_translation().x, 102), "grandchild offset applied");
    PT_ASSERT(has(w, "missing"), "unresolved child warned");

    std::vector<ObjectId> all;
    inst.collect_objects(all);
    PT_ASSERT_OP(all.size(), ==, size_t{3}, "2 model parts + cube; generic child is host-owned");

    FrameAllocator allocator(1024 * 1024);
    BatchBuilder builder(scene);
    builder.invalidate_all();
    builder.build(allocator);
    PT_ASSERT(has_batch_mesh(builder.batches(), 31) && has_batch_mesh(builder.batches(), 32),
              "resolved model part meshes reach render batches");
    PT_ASSERT(!has_batch_mesh(builder.batches(), INVALID_MESH),
              "generic/custom definitions never create invalid-mesh batches");
}

void test_bone_without_resolver_and_non_model_parent() {
    MemorySubsystem memory;
    SceneRegistry   scene(memory);
    VisusCatalog cat;
    VisusDesc g; g.name = "grp"; g.kind = VisusKind::GROUP;
    VisusChildRef c; c.visus = "leaf"; c.attach.bone = "Head";
    c.attach.offset[0] = 7.0f;
    g.children.push_back(c); cat.add(g);
    VisusDesc leaf; leaf.name = "leaf"; leaf.kind = VisusKind::TEXT; cat.add(leaf);

    VisusRuntime rt;
    VisusInstance inst;
    std::vector<std::string> w;
    const AABB bb = {{0, 0, 0}, {1, 1, 1}};
    PT_ASSERT(instantiate_visus(scene, cat, rt, "grp", translation(5, 0, 0), bb, inst, &w), "group root");
    PT_ASSERT(has(w, "non-model parent"), "bone on non-model parent warned");
    PT_ASSERT(near(inst.children[0].transform.get_translation().x, 5),
              "non-model bone fallback ignores offset and keeps parent transform");

    // model 親 + resolver 無し
    VisusDesc m; m.name = "m"; m.kind = VisusKind::MODEL; m.asset = "m.fbx";
    VisusChildRef cm; cm.visus = "leaf"; cm.attach.bone = "Head";
    cm.attach.offset[0] = 9.0f;
    m.children.push_back(cm); cat.add(m);
    VisusResolved rm; rm.name = "m"; rm.kind = VisusKind::MODEL; rm.model = 1;
    VisusResolvedPart rp; rp.part = "body"; rp.mesh = 41; rm.parts.push_back(rp); rt.set(rm);
    w.clear();
    instantiate_visus(scene, cat, rt, "m", float4x4::identity(), bb, inst, &w, nullptr);
    PT_ASSERT(has(w, "no resolver"), "bone without resolver warned");
    PT_ASSERT(near(inst.children[0].transform.get_translation().x, 0),
              "missing-resolver fallback ignores offset and keeps parent transform");
    PT_ASSERT_OP(inst.objects.size(), ==, size_t{1}, "resolved model part -> 1 object");

    // model 親 + resolver はあるが bone が見つからない
    VisusDesc missing; missing.name = "missing_bone"; missing.kind = VisusKind::MODEL;
    VisusChildRef cb; cb.visus = "leaf"; cb.attach.bone = "Unknown";
    cb.attach.offset[0] = 11.0f;
    missing.children.push_back(cb); cat.add(missing);
    VisusResolved rmissing; rmissing.name = "missing_bone"; rmissing.kind = VisusKind::MODEL;
    rmissing.model = 2; rt.set(rmissing);
    BoneResolver bones;
    w.clear();
    instantiate_visus(scene, cat, rt, "missing_bone", translation(3, 0, 0), bb,
                      inst, &w, &bones);
    PT_ASSERT(has(w, "not found"), "missing bone warned");
    PT_ASSERT(near(inst.children[0].transform.get_translation().x, 3),
              "missing-bone fallback ignores offset and keeps parent transform");
}

void test_cycle_and_missing_root() {
    MemorySubsystem memory;
    SceneRegistry   scene(memory);
    VisusCatalog cat;
    VisusDesc a; a.name = "a"; a.kind = VisusKind::GROUP; VisusChildRef ab; ab.visus = "b"; a.children.push_back(ab);
    VisusDesc b; b.name = "b"; b.kind = VisusKind::GROUP; VisusChildRef ba; ba.visus = "a"; b.children.push_back(ba);
    cat.add(a); cat.add(b);
    VisusRuntime rt;
    VisusInstance inst;
    std::vector<std::string> w;
    const AABB bb = {{0, 0, 0}, {1, 1, 1}};
    PT_ASSERT(instantiate_visus(scene, cat, rt, "a", float4x4::identity(), bb, inst, &w), "cyclic root still instantiates");
    PT_ASSERT(has(w, "visus cycle"), "cycle warned");
    PT_ASSERT(inst.children.size() == 1 && inst.children[0].children.empty(), "cycle cut at second level");
    PT_ASSERT(!instantiate_visus(scene, cat, rt, "zzz", float4x4::identity(), bb, inst, &w), "unknown root -> false");
    PT_ASSERT(has(w, "visus not found"), "unknown root warned");
}

// primitive は解決済みの shader / material も ObjectDescriptor へ運ぶ
// (model part と同じ契約)。 shader 未解決なら metadata の key_override を残す。
void test_primitive_carries_resolved_shader_and_material() {
    MemorySubsystem memory;
    SceneRegistry   scene(memory);
    VisusCatalog cat;
    VisusDesc shaded; shaded.name = "shaded"; shaded.kind = VisusKind::PRIMITIVE;
    cat.add(shaded);
    VisusDesc plain; plain.name = "plain"; plain.kind = VisusKind::PRIMITIVE;
    plain.metadata.set(visus_keys::kShaderKeyOverride, 12);
    cat.add(plain);

    VisusRuntime rt;
    VisusResolved rs; rs.name = "shaded"; rs.kind = VisusKind::PRIMITIVE;
    rs.mesh = 61; rs.shader = 4; rs.material = 17;
    rs.shader_key = ShaderKey::with_custom_shader(6, 4);
    rt.set(rs);
    VisusResolved rp; rp.name = "plain"; rp.kind = VisusKind::PRIMITIVE; rp.mesh = 62;
    rt.set(rp);   // shader 未解決 / shader_key 0 のまま

    VisusInstance inst;
    const AABB bb = {{0, 0, 0}, {1, 1, 1}};
    PT_ASSERT(instantiate_visus(scene, cat, rt, "shaded", float4x4::identity(), bb, inst),
              "shaded primitive instantiates");
    PT_ASSERT_OP(inst.objects.size(), ==, size_t{1}, "one primitive object");
    {
        const SceneRegistry::ObjectLocation loc = scene.find_object(inst.objects[0]);
        PT_ASSERT(loc.valid, "primitive registered");
        const auto& pool = scene.pool(loc.pool_type);
        PT_ASSERT_OP(pool.shader_keys()[loc.pool_index], ==, rs.shader_key,
                     "resolved primitive shader_key reaches the descriptor");
        PT_ASSERT(ShaderKey::is_custom(pool.shader_keys()[loc.pool_index]),
                  "resolved custom shader flag survives");
        PT_ASSERT_OP(pool.material_keys()[loc.pool_index], ==, uint32_t{17},
                     "resolved primitive material keys the batch");
    }

    PT_ASSERT(instantiate_visus(scene, cat, rt, "plain", float4x4::identity(), bb, inst),
              "plain primitive instantiates");
    {
        const SceneRegistry::ObjectLocation loc = scene.find_object(inst.objects[0]);
        PT_ASSERT_OP(scene.pool(loc.pool_type).shader_keys()[loc.pool_index], ==, uint64_t{12},
                     "unresolved shader keeps the metadata key_override");
    }
}

// 重ね掛けパス (§2.5.3): part ごとに base + パッケージ数の ObjectDescriptor が
// 出て、 binding から動的パラメータを書き換えられる。
void test_package_overlay_passes() {
    MemorySubsystem memory;
    SceneRegistry   scene(memory);

    VisusCatalog cat;
    VisusDesc d;
    d.name  = "kuzuha";
    d.kind  = VisusKind::MODEL;
    d.asset = "k.fbx";
    cat.add(d);

    VisusResolved r = resolved_model();
    VisusResolvedPackage toon;
    toon.package    = "toon";
    toon.shader     = 77;
    toon.shader_key = ShaderKey::with_custom_shader(0, 77);
    toon.params.set("rim_power", 2.0);
    VisusResolvedPackage outline;
    outline.package = "outline";
    outline.shader  = 78;
    r.parts[0].packages = {toon, outline};   // cloak: base + 2 パス
    r.parts[1].packages = {toon};            // face:  base + 1 パス

    VisusRuntime rt;
    rt.set(r);

    VisusInstance inst;
    std::vector<std::string> w;
    const AABB bb = {{-1, -1, -1}, {1, 1, 1}};
    PT_ASSERT(instantiate_visus(scene, cat, rt, "kuzuha", float4x4::identity(), bb, inst, &w),
              "instantiates");
    PT_ASSERT_OP(inst.objects.size(), ==, size_t{5}, "2 base + 3 overlay passes");
    PT_ASSERT_OP(inst.bindings.size(), ==, size_t{3}, "one binding per overlay pass");
    PT_ASSERT(inst.bindings[0].part == "cloak" && inst.bindings[0].package == "toon",
              "bindings follow registration order");
    PT_ASSERT(inst.bindings[1].package == "outline", "overlay order preserved");
    PT_ASSERT(inst.bindings[2].part == "face", "later parts come after");

    // 動的パラメータ: 正本は binding が持ち、 変更で改訂番号が上がる。
    PT_ASSERT_OP(inst.params_revision(), ==, uint64_t{0}, "fresh instance");
    PT_ASSERT(inst.set_param("cloak", "toon", "rim_power", VisusValue(5.0)), "set_param");
    PT_ASSERT_OP(inst.params_revision(), ==, uint64_t{1}, "revision bumped");
    const VisusPackageBinding* b = inst.find_binding("cloak", "toon");
    PT_ASSERT(b && b->params.get_number("rim_power").value_or(0.0) == 5.0, "value written");
    PT_ASSERT(inst.find_binding("face", "toon")->params.get_number("rim_power").value_or(0.0) == 2.0,
              "other bindings keep their own copy");
    PT_ASSERT(inst.set_param("cloak", "toon", "rim_power", VisusValue(5.0)), "same value ok");
    PT_ASSERT_OP(inst.params_revision(), ==, uint64_t{1}, "no bump when nothing changed");
    PT_ASSERT(!inst.set_param("cloak", "nope", "x", VisusValue(1.0)), "unknown package");
    PT_ASSERT(inst.find_binding("nope", "toon") == nullptr, "unknown part");
}

} // namespace

int main() {
    test_base_descriptor_defaults_and_metadata();
    test_compose();
    test_model_parts_and_children();
    test_bone_without_resolver_and_non_model_parent();
    test_cycle_and_missing_root();
    test_primitive_carries_resolved_shader_and_material();
    test_package_overlay_passes();
    return report("unit_visus_instantiator_test");
}
