/// VisusRuntime — IVisusResolver 注入で name → handle / part 別 shaderKey が埋まる。

#include "pictor/visus/visus_catalog.h"
#include "pictor/visus/visus_package_catalog.h"
#include "pictor/visus/visus_runtime.h"
#include "test_common.h"

#include <algorithm>
#include <map>
#include <string>
#include <vector>

using namespace pictor;
using namespace pictor_test;

namespace {

// 呼び出しを記録し、 パスごとに連番 handle を返す偽 resolver。
struct FakeResolver : IVisusResolver {
    std::vector<std::string> models, meshes, shaders, materials, generics;
    std::map<std::string, float4x4> bones;   // "<bone>" → 行列
    bool fail_shader = false;

    ModelHandle load_model(const std::string& path, std::string*) override {
        models.push_back(path);
        return static_cast<ModelHandle>(100 + models.size());
    }
    std::vector<VisusModelPart> model_parts(ModelHandle, std::string*) override {
        return {
            {"T_Cloak_bsc", 501},
            {"T_Face_bsc",  502},
            {"T_Hair_bsc",  503},
            {"T_Shoes_bsc", 504},
        };
    }
    MeshHandle load_mesh(const std::string& path, std::string*) override {
        meshes.push_back(path);
        return static_cast<MeshHandle>(200 + meshes.size());
    }
    ShaderHandle register_shader_stages(const VisusShaderRef& st, const VisusMetadata&,
                                        std::string* err) override {
        if (fail_shader) { if (err) *err = "spv missing"; return INVALID_SHADER; }
        shaders.push_back(st.vert + "|" + st.frag);
        return static_cast<ShaderHandle>(shaders.size());   // 1, 2, ...
    }
    ShaderHandle builtin_shader(std::string_view name) override {
        return name == "toon" ? ShaderHandle{42} : INVALID_SHADER;
    }
    MaterialHandle load_material(const std::string& path, std::string*) override {
        materials.push_back(path);
        return static_cast<MaterialHandle>(300 + materials.size());
    }
    uint32_t load_generic(VisusKind, const std::string& path, std::string*) override {
        generics.push_back(path);
        return static_cast<uint32_t>(400 + generics.size());
    }
    bool bone_transform(ModelHandle, std::string_view bone, float4x4& out) override {
        auto it = bones.find(std::string(bone));
        if (it == bones.end()) return false;
        out = it->second;
        return true;
    }
};

VisusDesc model_desc() {
    VisusDesc d;
    d.name  = "kuzuha";
    d.kind  = VisusKind::MODEL;
    d.asset = "models/kuzuha.fbx";
    VisusPart pbr;  pbr.part = "T_Cloak_bsc";  pbr.shader = VisusShaderRef::builtin();
    VisusPart face; face.part = "T_Face_bsc";  face.shader = VisusShaderRef::stages("sh/face.vert.spv", "sh/face.frag.spv");
    face.metadata.set(visus_keys::kShaderKeyOverride, 5);
    face.metadata.set(visus_keys::kMaterial, "mat/face.mat.json");
    VisusPart toon; toon.part = "T_Hair_bsc";  toon.shader = VisusShaderRef::builtin("toon");
    VisusPart shared; shared.part = "*";        shared.shader = VisusShaderRef::visus("fx_shader");
    d.parts = {pbr, face, toon, shared};
    d.metadata.set(visus_keys::kShaderKeyOverride, 9);
    VisusChildRef facial; facial.visus = "kuzuha_facial"; facial.attach.bone = "Head";
    d.children.push_back(facial);
    return d;
}

VisusDesc custom_desc(const char* name, const char* vert, const char* frag) {
    VisusDesc d;
    d.name = name;
    d.kind = VisusKind::CUSTOM;
    d.metadata.set(visus_keys::kShader, visus_shader_ref_to_value(VisusShaderRef::stages(vert, frag)));
    d.metadata.set(visus_keys::kShaderKeyOverride, 3);
    return d;
}

bool has(const std::vector<std::string>& v, const char* needle) {
    return std::any_of(v.begin(), v.end(), [&](const std::string& s) { return s.find(needle) != std::string::npos; });
}

void test_model_parts_and_children() {
    VisusCatalog cat;
    cat.add(model_desc());
    cat.add(custom_desc("fx_shader", "sh/fx.vert.spv", "sh/fx.frag.spv"));
    VisusDesc facial; facial.name = "kuzuha_facial"; facial.kind = VisusKind::RIVE; facial.asset = "rive/face.riv";
    cat.add(facial);

    FakeResolver rs;
    VisusRuntime rt;
    std::vector<std::string> warnings;
    PT_ASSERT(rt.resolve(cat, "kuzuha", rs, &warnings), "resolve root");
    PT_ASSERT(warnings.empty(), "no warnings on healthy tree");

    const VisusResolved* r = rt.get("kuzuha");
    PT_ASSERT(r && r->kind == VisusKind::MODEL, "root resolved");
    PT_ASSERT(r->model == 101, "model handle from resolver");
    PT_ASSERT(rs.models.size() == 1 && rs.models[0] == "models/kuzuha.fbx", "asset path passed (no source dir → as-is)");
    PT_ASSERT_OP(r->parts.size(), ==, size_t{4}, "4 parts resolved in order");
    PT_ASSERT(r->parts[0].part == "T_Cloak_bsc" && r->parts[0].mesh == 501 &&
              r->parts[3].part == "T_Shoes_bsc" && r->parts[3].mesh == 504,
              "runtime parts carry resolver draw meshes; wildcard applies to unlisted part");
    PT_ASSERT(r->find_part("T_Face_bsc") && r->find_part("T_Face_bsc")->mesh == 502 &&
              r->find_part("missing") == nullptr, "resolved part lookup uses actual draw names");

    // builtin:pbr → INVALID_SHADER、 shaderKey = visus の key_override (9)、 custom ビット無し
    PT_ASSERT(r->parts[0].shader == INVALID_SHADER, "builtin:pbr has no handle");
    PT_ASSERT_OP(r->parts[0].shader_key, ==, uint64_t{9}, "pbr part key = visus key_override");
    PT_ASSERT(!ShaderKey::is_custom(r->parts[0].shader_key), "pbr part not custom");

    // STAGES → registered handle 1、 part key_override (5) が visus (9) に優先、 custom ビット
    PT_ASSERT(r->parts[1].shader == 1, "stages registered");
    PT_ASSERT(ShaderKey::is_custom(r->parts[1].shader_key) &&
              ShaderKey::custom_shader(r->parts[1].shader_key) == 1 &&
              (r->parts[1].shader_key & 0xFFFFFFFFULL) == 5,
              "part shader_key = part override + custom handle");
    PT_ASSERT(r->parts[1].material == 301 && rs.materials[0] == "mat/face.mat.json", "part material loaded");

    // builtin:toon → host の 42
    PT_ASSERT(r->parts[2].shader == 42 && ShaderKey::custom_shader(r->parts[2].shader_key) == 42,
              "non-default builtin via host");

    // visus:fx_shader → fx_shader の STAGES を登録 (handle 2)、 fx_shader 自身も side-table に入る
    PT_ASSERT(r->parts[3].shader == 2, "visus: reference registers target stages");
    const VisusResolved* fx = rt.get("fx_shader");
    PT_ASSERT(fx && fx->shader == 2 && ShaderKey::custom_shader(fx->shader_key) == 2 &&
              (fx->shader_key & 0xFFFFFFFFULL) == 3, "shared shader visus cached with its own key_override");
    PT_ASSERT_OP(rs.shaders.size(), ==, size_t{2}, "exactly 2 shader registrations (face + fx)");

    // children も再帰的に解決
    const VisusResolved* f = rt.get("kuzuha_facial");
    PT_ASSERT(f && f->kind == VisusKind::RIVE && f->generic_handle == 401, "child rive resolved via load_generic");
    PT_ASSERT_OP(rt.size(), ==, size_t{3}, "kuzuha + fx_shader + kuzuha_facial");

    // 再 resolve は no-op (resolver を呼び直さない)
    rt.resolve(cat, "kuzuha", rs, &warnings);
    PT_ASSERT_OP(rs.models.size(), ==, size_t{1}, "already resolved → not reloaded");
    PT_ASSERT(rt.invalidate("kuzuha") && rt.size() == 0,
              "invalidating a root also drops child and shared-shader dependencies");

    PT_ASSERT(rt.resolve(cat, "kuzuha", rs, &warnings), "resolve invalidated tree");
    PT_ASSERT_OP(rs.models.size(), ==, size_t{2}, "root model reloaded");
    PT_ASSERT_OP(rs.generics.size(), ==, size_t{2}, "child asset reloaded");
    PT_ASSERT_OP(rs.shaders.size(), ==, size_t{4}, "direct and shared shaders re-registered");
    PT_ASSERT(rt.get("fx_shader") && rt.get("kuzuha_facial"),
              "dependent entries repopulated with the refreshed tree");
}

void test_custom_kind_and_errors() {
    VisusCatalog cat;
    cat.add(custom_desc("demo", "a.vert.spv", "a.frag.spv"));
    VisusDesc no_shader; no_shader.name = "bare"; no_shader.kind = VisusKind::CUSTOM;
    cat.add(no_shader);
    VisusDesc huge = custom_desc("huge", "h.vert.spv", "h.frag.spv");
    huge.metadata.set(visus_keys::kShaderKeyOverride, 1e300);
    cat.add(huge);
    VisusDesc bad_ref; bad_ref.name = "bad"; bad_ref.kind = VisusKind::MODEL;
    bad_ref.asset = "bad.fbx";
    VisusPart p; p.part = "*"; p.shader = VisusShaderRef::visus("nope"); bad_ref.parts.push_back(p);
    VisusPart q; q.part = "T_Face_bsc"; q.shader = VisusShaderRef::visus("bare"); bad_ref.parts.push_back(q);
    cat.add(bad_ref);

    FakeResolver rs;
    VisusRuntime rt;
    std::vector<std::string> w;
    PT_ASSERT(rt.resolve(cat, "demo", rs, &w), "custom resolves");
    const VisusResolved* d = rt.get("demo");
    PT_ASSERT(d && d->shader == 1 && ShaderKey::custom_shader(d->shader_key) == 1 &&
              (d->shader_key & 0xFFFFFFFFULL) == 3, "custom shader + key override");

    PT_ASSERT(rt.resolve(cat, "huge", rs, &w), "out-of-range key still resolves safely");
    const VisusResolved* h = rt.get("huge");
    PT_ASSERT(h && ShaderKey::is_custom(h->shader_key) &&
              (h->shader_key & 0xFFFFFFFFULL) == 0,
              "out-of-range shader.key_override is rejected before uint64 conversion");

    w.clear();
    rt.resolve(cat, "bad", rs, &w);
    PT_ASSERT(has(w, "shader visus not found"), "unknown visus: ref warns");
    PT_ASSERT(has(w, "has no metadata"), "visus: ref to custom without shader warns");
    PT_ASSERT(rt.get("bad")->parts[0].shader == INVALID_SHADER, "unresolved part falls back to INVALID");

    w.clear();
    rt.invalidate("bare");   // visus: 参照経由で side-table に入ったものを捨てて直接解決
    rt.resolve(cat, "bare", rs, &w);
    PT_ASSERT(has(w, "without metadata"), "custom without shader warns");
    PT_ASSERT(rt.get("bare") && rt.get("bare")->shader == INVALID_SHADER, "still registered, no handle");

    // resolver 失敗は warning + INVALID
    VisusRuntime rt2; FakeResolver rs2; rs2.fail_shader = true;
    std::vector<std::string> w2;
    rt2.resolve(cat, "demo", rs2, &w2);
    PT_ASSERT(has(w2, "spv missing"), "resolver error surfaced");
    PT_ASSERT(rt2.get("demo")->shader == INVALID_SHADER, "failed shader is INVALID");

    PT_ASSERT(!rt.resolve(cat, "missing", rs, &w), "unknown root → false");
}

void test_cycle_and_primitive() {
    VisusCatalog cat;
    VisusDesc a; a.name = "a"; a.kind = VisusKind::GROUP; VisusChildRef ab; ab.visus = "b"; a.children.push_back(ab);
    VisusDesc b; b.name = "b"; b.kind = VisusKind::PRIMITIVE; b.asset = "m/cube.mesh"; VisusChildRef ba; ba.visus = "a"; b.children.push_back(ba);
    cat.add(a); cat.add(b);
    FakeResolver rs; VisusRuntime rt; std::vector<std::string> w;
    PT_ASSERT(rt.resolve(cat, "a", rs, &w), "root ok despite cycle below");
    PT_ASSERT(has(w, "visus cycle"), "cycle warned");
    PT_ASSERT(rt.get("b") && rt.get("b")->mesh == 201, "primitive mesh resolved once");
    PT_ASSERT_OP(rs.meshes.size(), ==, size_t{1}, "mesh loaded once");
}

void test_default_resolver_reports_unsupported_capability() {
    VisusCatalog cat;
    VisusDesc primitive;
    primitive.name = "primitive";
    primitive.kind = VisusKind::PRIMITIVE;
    primitive.asset = "cube.mesh";
    cat.add(primitive);
    VisusDesc custom;
    custom.name = "custom";
    custom.kind = VisusKind::CUSTOM;
    custom.metadata.set(visus_keys::kShader,
                        visus_shader_ref_to_value(VisusShaderRef::builtin("toon")));
    cat.add(custom);

    IVisusResolver resolver;
    VisusRuntime runtime;
    std::vector<std::string> warnings;
    PT_ASSERT(runtime.resolve(cat, "primitive", resolver, &warnings),
              "unsupported resolver capability still records resolution result");
    PT_ASSERT(has(warnings, "mesh loading is not supported"),
              "default resolver failure is observable");
    PT_ASSERT(runtime.get("primitive") && runtime.get("primitive")->mesh == INVALID_MESH,
              "unsupported mesh remains invalid");

    warnings.clear();
    PT_ASSERT(runtime.resolve(cat, "custom", resolver, &warnings),
              "unsupported named builtin records resolution result");
    PT_ASSERT(has(warnings, "builtin shader unavailable: toon"),
              "unsupported named builtin fallback is observable");
}

// metadata は ShaderKey の下位 32bit しか渡せない。 上位は CUSTOM フラグ +
// ShaderHandle の予約領域で、 `with_custom_shader` は shader が INVALID の
// とき base をそのまま返すため、 刈らないと untrusted な visus ファイルが
// 「解決済みカスタムシェーダ」を捏造できる。
void test_key_override_cannot_forge_custom_shader() {
    VisusCatalog cat;
    VisusDesc spoof;
    spoof.name  = "spoof";
    spoof.kind  = VisusKind::MODEL;
    spoof.asset = "spoof.fbx";
    VisusPart wildcard;
    wildcard.part   = "*";
    wildcard.shader = VisusShaderRef::builtin();   // builtin:pbr → INVALID_SHADER
    spoof.parts.push_back(wildcard);
    // bit 63 (CUSTOM_FLAG) + handle ビットを立てた値
    spoof.metadata.set(visus_keys::kShaderKeyOverride,
                       static_cast<double>(ShaderKey::CUSTOM_FLAG | (uint64_t{7} << 32) | 11));
    cat.add(spoof);

    VisusDesc edge;
    edge.name  = "edge";
    edge.kind  = VisusKind::MODEL;
    edge.asset = "edge.fbx";
    edge.parts.push_back(wildcard);
    edge.metadata.set(visus_keys::kShaderKeyOverride, 4294967296.0);   // uint32 max + 1
    cat.add(edge);

    FakeResolver rs;
    VisusRuntime rt;
    std::vector<std::string> w;
    PT_ASSERT(rt.resolve(cat, "spoof", rs, &w), "spoofed key still resolves");
    const VisusResolved* s = rt.get("spoof");
    PT_ASSERT(s && !s->parts.empty(), "parts resolved");
    for (const VisusResolvedPart& p : s->parts) {
        PT_ASSERT(p.shader == INVALID_SHADER, "builtin:pbr part has no shader handle");
        PT_ASSERT(!ShaderKey::is_custom(p.shader_key),
                  "metadata shader.key_override cannot set the custom-shader flag");
        PT_ASSERT_OP(p.shader_key, ==, uint64_t{0},
                     "out-of-range shader.key_override falls back to 0");
    }
    PT_ASSERT(!ShaderKey::is_custom(s->shader_key), "visus-level key cannot forge custom either");

    PT_ASSERT(rt.resolve(cat, "edge", rs, &w), "uint32-overflow key resolves");
    PT_ASSERT_OP(rt.get("edge")->parts[0].shader_key, ==, uint64_t{0},
                 "key_override above uint32 max is rejected");
}

// `visus:` 参照先が既に side-table にいても kind=custom 検査は効く
// (参照先が先に解決された順序で診断が消えない)。
void test_visus_shader_ref_rejects_cached_non_custom() {
    VisusCatalog cat;
    VisusDesc base;
    base.name  = "base_model";
    base.kind  = VisusKind::MODEL;
    base.asset = "base.fbx";
    cat.add(base);

    VisusDesc user;
    user.name  = "user";
    user.kind  = VisusKind::MODEL;
    user.asset = "user.fbx";
    VisusPart p;
    p.part   = "*";
    p.shader = VisusShaderRef::visus("base_model");
    user.parts.push_back(p);
    cat.add(user);

    FakeResolver rs;
    VisusRuntime rt;
    std::vector<std::string> w;
    rt.resolve(cat, "base_model", rs, &w);   // 先に解決 → side-table に載る
    w.clear();
    rt.resolve(cat, "user", rs, &w);
    PT_ASSERT(has(w, "is not kind=custom"),
              "cached non-custom visus: target is still reported");
    PT_ASSERT(rt.get("user") && rt.get("user")->parts[0].shader == INVALID_SHADER,
              "non-custom visus: reference yields no shader");
}

// ---- シェーダーパッケージ (§2.5) ---------------------------------------------

VisusShaderPackage package(const char* name, VisusShaderRef shader) {
    VisusShaderPackage p;
    p.name   = name;
    p.shader = std::move(shader);
    return p;
}

VisusPackageRef assign(const char* name) {
    VisusPackageRef r;
    r.package = name;
    return r;
}

void test_shader_packages() {
    VisusPackageCatalog packages;
    VisusShaderPackage toon = package("toon", VisusShaderRef::builtin("toon"));
    toon.params.set("rim_power", 2.0);
    toon.params.set("tint", 1.0);
    packages.add(std::move(toon));

    VisusShaderPackage outline =
        package("outline", VisusShaderRef::stages("sh/outline.vert.spv", "sh/outline.frag.spv"));
    outline.params.set("width", 0.01);
    outline.metadata.set(visus_keys::kShaderKeyOverride, 6);
    packages.add(std::move(outline));

    VisusDesc d = model_desc();
    d.packages = {assign("toon"), assign("outline")};
    // T_Face_bsc だけ outline を外し、 toon の rim_power を上書きする。
    VisusPackageRef face_off = assign("outline");
    face_off.enabled = false;
    VisusPackageRef face_toon = assign("toon");
    face_toon.params.set("rim_power", 9.0);
    for (VisusPart& p : d.parts) {
        if (p.part == "T_Face_bsc") p.packages = {face_off, face_toon};
    }
    // 未知パッケージのアサインは警告して飛ばす。
    for (VisusPart& p : d.parts) {
        if (p.part == "T_Hair_bsc") p.packages = {assign("nope")};
    }

    VisusCatalog cat;
    cat.add(d);
    FakeResolver rs;
    VisusRuntime rt;
    std::vector<std::string> w;
    PT_ASSERT(rt.resolve(cat, "kuzuha", rs, &w, &packages), "resolve with a package catalog");

    const VisusResolved* r = rt.get("kuzuha");
    PT_ASSERT(r != nullptr, "resolved");
    const VisusResolvedPart* cloak = r->find_part("T_Cloak_bsc");
    PT_ASSERT(cloak != nullptr, "cloak resolved");
    PT_ASSERT_OP(cloak->packages.size(), ==, size_t{2}, "visus level packages apply to every part");
    PT_ASSERT(cloak->packages[0].package == "toon" && cloak->packages[1].package == "outline",
              "assignment order preserved");
    PT_ASSERT_OP(cloak->packages[0].shader, ==, ShaderHandle{42}, "builtin:toon resolved");
    PT_ASSERT(cloak->packages[1].shader != INVALID_SHADER, "outline stages registered");
    PT_ASSERT_OP(cloak->packages[0].params.get_number("rim_power").value_or(0.0), ==, 2.0,
                 "package default params");
    PT_ASSERT_OP(cloak->packages[1].shader_key, ==,
                 ShaderKey::with_custom_shader(6, cloak->packages[1].shader),
                 "package metadata key_override feeds the shader key");

    const VisusResolvedPart* face = r->find_part("T_Face_bsc");
    PT_ASSERT(face != nullptr, "face resolved");
    PT_ASSERT_OP(face->packages.size(), ==, size_t{1}, "part level disabled the outline pass");
    PT_ASSERT(face->packages[0].package == "toon", "remaining pass is toon");
    PT_ASSERT_OP(face->packages[0].params.get_number("rim_power").value_or(0.0), ==, 9.0,
                 "part level params override the package default");
    PT_ASSERT_OP(face->packages[0].params.get_number("tint").value_or(0.0), ==, 1.0,
                 "untouched defaults survive");
    PT_ASSERT(has(w, "shader package not found"), "unknown package warned");

    // base シェーダは従来通り part の typed shader のまま。
    PT_ASSERT_OP(cloak->shader, ==, INVALID_SHADER, "base pass still builtin:pbr");
}

void test_shader_packages_need_a_catalog() {
    VisusDesc d = model_desc();
    d.packages = {assign("toon")};
    VisusCatalog cat;
    cat.add(d);
    FakeResolver rs;
    VisusRuntime rt;
    std::vector<std::string> w;
    PT_ASSERT(rt.resolve(cat, "kuzuha", rs, &w), "resolves without a package catalog");
    PT_ASSERT(has(w, "no package catalog"), "missing package catalog warned");
    const VisusResolved* r = rt.get("kuzuha");
    PT_ASSERT(r && r->find_part("T_Cloak_bsc") && r->find_part("T_Cloak_bsc")->packages.empty(),
              "no overlay passes without a catalog");
}

// シェーダが解決できないパッケージは重ね掛けパスを作らない。 積むと
// instantiate 側 (register_packages) が base と同一の ObjectDescriptor を
// 登録し、 同じ mesh が base pipeline で二重描画される。
void test_shader_package_with_unresolved_shader_is_dropped() {
    VisusPackageCatalog packages;
    packages.add(package("broken", VisusShaderRef::builtin("nonexistent")));
    packages.add(package("plain", VisusShaderRef::builtin()));   // 既定 = 意図的

    VisusDesc d;
    d.name     = "cube";
    d.kind     = VisusKind::PRIMITIVE;
    d.asset    = "cube.mesh";
    d.packages = {assign("broken"), assign("plain")};
    VisusCatalog cat;
    cat.add(d);

    FakeResolver rs;
    VisusRuntime rt;
    std::vector<std::string> w;
    PT_ASSERT(rt.resolve(cat, "cube", rs, &w, &packages), "primitive resolves");
    const VisusResolved* r = rt.get("cube");
    PT_ASSERT(r != nullptr, "resolved");
    PT_ASSERT_OP(r->packages.size(), ==, size_t{1},
                 "unresolvable package shader drops the overlay pass");
    PT_ASSERT(r->packages[0].package == "plain",
              "an explicit default-builtin assignment is not a failure");
    PT_ASSERT(has(w, "pass dropped"), "dropped overlay pass is warned");
}

void test_shader_packages_on_primitive() {
    VisusPackageCatalog packages;
    packages.add(package("toon", VisusShaderRef::builtin("toon")));

    VisusDesc d;
    d.name  = "cube";
    d.kind  = VisusKind::PRIMITIVE;
    d.asset = "cube.mesh";
    d.packages = {assign("toon")};
    VisusCatalog cat;
    cat.add(d);

    FakeResolver rs;
    VisusRuntime rt;
    std::vector<std::string> w;
    PT_ASSERT(rt.resolve(cat, "cube", rs, &w, &packages), "primitive resolves");
    const VisusResolved* r = rt.get("cube");
    PT_ASSERT(r != nullptr, "resolved");
    PT_ASSERT_OP(r->packages.size(), ==, size_t{1}, "non-model kinds carry the assignments");
    PT_ASSERT_OP(r->packages[0].shader, ==, ShaderHandle{42}, "builtin:toon resolved");
}

// primitive は parts[] を持てないので、 material は visus metadata から解決する
// (v1 変換は非 model の materials[] をここへ落とす)。
void test_primitive_material_is_resolved() {
    VisusCatalog cat;
    VisusDesc prim;
    prim.name  = "cube";
    prim.kind  = VisusKind::PRIMITIVE;
    prim.asset = "m/cube.mesh";
    prim.metadata.set(visus_keys::kMaterial, "mat/cube.mat.json");
    cat.add(prim);

    FakeResolver rs;
    VisusRuntime rt;
    std::vector<std::string> w;
    PT_ASSERT(rt.resolve(cat, "cube", rs, &w), "primitive resolves");
    const VisusResolved* r = rt.get("cube");
    PT_ASSERT(r && r->mesh == 201, "primitive mesh resolved");
    PT_ASSERT(r->material == 301, "primitive visus-level material resolved");
    PT_ASSERT(rs.materials.size() == 1 && rs.materials[0] == "mat/cube.mat.json",
              "material path passed through resolve_path");

    // material metadata が無い primitive は resolver を呼ばない。
    VisusDesc bare; bare.name = "bare_cube"; bare.kind = VisusKind::PRIMITIVE; bare.asset = "m/b.mesh";
    cat.add(bare);
    rt.resolve(cat, "bare_cube", rs, &w);
    PT_ASSERT(rt.get("bare_cube")->material == INVALID_MATERIAL, "no material metadata -> INVALID");
    PT_ASSERT_OP(rs.materials.size(), ==, size_t{1}, "no extra material load");
}

// カタログは木ではなく DAG。 共有された部分木を親ごとに歩き直すと訪問数が
// branching^depth になり、 同じ警告がその回数だけ積まれる。 (name, depth) を
// 1 回だけ訪れること (VisusCatalog::validate_node_ と同じ不変条件)。
void test_shared_subtree_is_visited_once_per_depth() {
    VisusCatalog cat;
    constexpr int kLevels = VisusCatalog::kMaxChildDepth;   // lv0 .. lv8
    for (int i = 0; i <= kLevels; ++i) {
        VisusDesc d;
        d.name = "lv" + std::to_string(i);
        d.kind = VisusKind::GROUP;
        if (i < kLevels) {
            // 同じ子を 2 回参照する = 各段で分岐 2。 memo が無いと 2^8 = 256 訪問。
            VisusChildRef c; c.visus = "lv" + std::to_string(i + 1);
            d.children.push_back(c);
            d.children.push_back(c);
        } else {
            VisusChildRef ghost; ghost.visus = "ghost";   // 解決不能 = 訪問ごとに警告
            d.children.push_back(ghost);
        }
        cat.add(d);
    }

    FakeResolver rs;
    VisusRuntime rt;
    std::vector<std::string> w;
    PT_ASSERT(rt.resolve(cat, "lv0", rs, &w), "shared-subtree root resolves");

    size_t ghost_warnings = 0;
    for (const std::string& msg : w)
        if (msg.find("ghost") != std::string::npos) ++ghost_warnings;
    PT_ASSERT_OP(ghost_warnings, ==, size_t{1},
                 "deepest shared node is walked once, not once per path");
    PT_ASSERT_OP(rt.size(), ==, static_cast<size_t>(kLevels + 1),
                 "every level resolved exactly once");
}

} // namespace

int main() {
    test_model_parts_and_children();
    test_custom_kind_and_errors();
    test_cycle_and_primitive();
    test_default_resolver_reports_unsupported_capability();
    test_key_override_cannot_forge_custom_shader();
    test_visus_shader_ref_rejects_cached_non_custom();
    test_primitive_material_is_resolved();
    test_shared_subtree_is_visited_once_per_depth();
    test_shader_packages();
    test_shader_packages_need_a_catalog();
    test_shader_package_with_unresolved_shader_is_dropped();
    test_shader_packages_on_primitive();
    return report("unit_visus_runtime_test");
}
