/// Visus v2 — JSON round-trip / 未知 key 保持 / parts・children / v1 -> v2 変換。

#include "pictor/visus/visus.h"
#include "pictor/visus/visus_serializer.h"
#include "test_common.h"

#include <algorithm>
#include <string>
#include <vector>

using namespace pictor;
using namespace pictor_test;

namespace {

VisusDesc make_sample() {
    VisusDesc d;
    d.name  = "kuzuha";
    d.kind  = VisusKind::MODEL;
    d.asset = "../../KzSUnity/Assets/3D/Characters/ch_Kuzuha_00/ch_Kuzuha_0000.fbx";

    VisusPart cloak;
    cloak.part   = "T_Cloak_bsc";
    cloak.shader = VisusShaderRef::builtin();
    cloak.metadata.set("texture.diffuse", "../tex/T_Cloak_bsc.png");
    d.parts.push_back(cloak);

    VisusPart face;
    face.part   = "T_Face_bsc";
    face.shader = VisusShaderRef::stages("../shaders/face.vert.spv", "../shaders/face.frag.spv");
    face.metadata.set("cull", "none");
    d.parts.push_back(face);

    VisusPart rest;
    rest.part   = "*";
    rest.shader = VisusShaderRef::visus("toon_shader");
    d.parts.push_back(rest);

    VisusChildRef facial;
    facial.visus       = "kuzuha_facial";
    facial.attach.bone = "Head";
    facial.attach.offset[1] = 0.02f;
    facial.attach.offset[2] = 0.05f;
    facial.metadata.set("layer", "overlay");
    d.children.push_back(facial);

    VisusChildRef weapon;
    weapon.visus = "./weapons/katana.visus.json";
    d.children.push_back(weapon);

    d.metadata.set(visus_keys::kAnimationDefault, "Idle");
    d.metadata.set(visus_keys::kAnimationLoop, true);
    VisusValue::Array clips;
    clips.emplace_back("Animations/run.fbx");
    clips.emplace_back("Animations/attack.fbx");
    d.metadata.set(visus_keys::kAnimationClips, VisusValue(clips));
    d.metadata.set(visus_keys::kRenderFlags, 2);
    d.metadata.set(visus_keys::kScaleTargetHeight, 1.6);
    VisusMetadata nested;
    nested.set("a", 1);
    nested.set("b", "x");
    d.metadata.set("host.custom", VisusValue(nested));   // 未知 key (ホスト独自)
    return d;
}

bool same_desc(const VisusDesc& a, const VisusDesc& b) {
    if (a.name != b.name || a.kind != b.kind || a.asset != b.asset) return false;
    if (a.parts.size() != b.parts.size()) return false;
    for (size_t i = 0; i < a.parts.size(); ++i) {
        if (a.parts[i].part != b.parts[i].part) return false;
        if (a.parts[i].shader != b.parts[i].shader) return false;
        if (a.parts[i].metadata != b.parts[i].metadata) return false;
    }
    if (a.children.size() != b.children.size()) return false;
    for (size_t i = 0; i < a.children.size(); ++i) {
        if (a.children[i].visus != b.children[i].visus) return false;
        if (a.children[i].attach.bone != b.children[i].attach.bone) return false;
        for (int k = 0; k < 3; ++k)
            if (a.children[i].attach.offset[k] != b.children[i].attach.offset[k]) return false;
        if (a.children[i].metadata != b.children[i].metadata) return false;
    }
    return a.metadata == b.metadata;
}

bool has_warning(const std::vector<std::string>& w, const char* needle) {
    return std::any_of(w.begin(), w.end(), [&](const std::string& s) {
        return s.find(needle) != std::string::npos;
    });
}

void test_v2_round_trip() {
    const VisusDesc src = make_sample();
    const std::string json = to_visus_json(src);
    PT_ASSERT(json.find("\"version\": 2") != std::string::npos, "writes version 2");
    PT_ASSERT(json.find("handle:") == std::string::npos, "no resolved handles in JSON");

    VisusDesc back;
    std::string err;
    std::vector<std::string> warnings;
    const bool ok = from_visus_json(json, back, &err, &warnings);
    PT_ASSERT(ok, "v2 parse succeeds");
    PT_ASSERT(warnings.empty(), "no warnings on clean v2");
    PT_ASSERT(same_desc(src, back), "v2 round-trip preserves all fields");

    // 二度目の emit は byte 同一 (canonical)
    PT_ASSERT(to_visus_json(back) == json, "canonical re-emit is stable");
}

void test_unknown_keys_preserved() {
    const char* json = R"({
      "version": 2, "name": "x", "kind": "rive", "asset": "a.riv",
      "metadata": { "future.key": [1, {"deep": true}], "rive.artboard": "main" }
    })";
    VisusDesc d;
    PT_ASSERT(from_visus_json(json, d), "parse unknown metadata keys");
    const VisusValue::Array* arr = d.metadata.get_array("future.key");
    PT_ASSERT(arr && arr->size() == 2 && (*arr)[1].as_object().get_bool("deep").value_or(false),
              "unknown nested value preserved");
    const std::string out = to_visus_json(d);
    PT_ASSERT(out.find("future.key") != std::string::npos, "unknown key re-emitted");
    PT_ASSERT(d.kind == VisusKind::RIVE && d.asset == "a.riv", "typed fields read");
}

void test_invalid_parts_are_ignored() {
    const char* json = R"({
      "version": 2, "name": "x", "kind": "model",
      "parts": [17, {"shader":"builtin:pbr"}, {"part":"body"}]
    })";
    VisusDesc d;
    std::vector<std::string> warnings;
    PT_ASSERT(from_visus_json(json, d, nullptr, &warnings), "permissive part parsing succeeds");
    PT_ASSERT_OP(d.parts.size(), ==, size_t{1}, "only valid named part retained");
    PT_ASSERT(d.parts[0].part == "body", "valid part retained");
    PT_ASSERT(has_warning(warnings, "non-object") && has_warning(warnings, "without 'part'"),
              "invalid parts are observable");
}

void test_find_part_and_shader_ref() {
    const VisusDesc d = make_sample();
    PT_ASSERT(d.find_part("T_Face_bsc")->shader.kind == VisusShaderRef::Kind::STAGES, "exact part");
    PT_ASSERT(d.find_part("unknown")->part == "*", "falls back to wildcard");
    VisusDesc no_wild;
    PT_ASSERT(no_wild.find_part("x") == nullptr, "no parts -> nullptr");

    VisusShaderRef r;
    PT_ASSERT(visus_shader_ref_from_value(VisusValue("builtin:toon"), r) &&
              r.kind == VisusShaderRef::Kind::BUILTIN && r.name == "toon", "builtin:");
    PT_ASSERT(visus_shader_ref_from_value(VisusValue("visus:fx"), r) &&
              r.kind == VisusShaderRef::Kind::VISUS && r.name == "fx", "visus:");
    PT_ASSERT(!visus_shader_ref_from_value(VisusValue("builtin:"), r), "empty builtin rejected");
    VisusMetadata incomplete_stages;
    incomplete_stages.set("vert", "only.vert.spv");
    PT_ASSERT(!visus_shader_ref_from_value(VisusValue(incomplete_stages), r),
              "stage object requires vert and frag");
    PT_ASSERT(!visus_shader_ref_from_value(VisusValue("garbage"), r), "unknown string rejected");
    PT_ASSERT(!visus_shader_ref_from_value(VisusValue(3), r), "number rejected");
}

void test_custom_kind_shader_in_metadata() {
    const char* json = R"({
      "version": 2, "name": "demo", "kind": "custom",
      "metadata": {
        "shader": { "vert": "../shaders/demo.vert.spv", "frag": "../shaders/demo.frag.spv" },
        "shader.key_override": 42,
        "shader.vertex_layout": { "stride": 32, "attributes": [ { "semantic": "position", "type": "float3", "offset": 0 } ] }
      }
    })";
    VisusDesc d;
    PT_ASSERT(from_visus_json(json, d), "parse custom");
    VisusShaderRef r;
    const VisusValue* sv = d.metadata.find(visus_keys::kShader);
    PT_ASSERT(sv && visus_shader_ref_from_value(*sv, r) && r.has_graphics_stages(),
              "metadata[shader] resolves to STAGES");
    PT_ASSERT(d.metadata.get_number(visus_keys::kShaderKeyOverride).value_or(0) == 42.0, "key override");
    const VisusMetadata* vl = d.metadata.get_object(visus_keys::kShaderVertexLayout);
    PT_ASSERT(vl && vl->get_number("stride").value_or(0) == 32.0, "vertex layout kept as metadata");
}

// v1 (KS data/visus/knife_rabbit.visus.json 相当) -> v2 変換。
void test_v1_conversion_model() {
    const char* v1 = R"({
      "version": 1,
      "name": "knife_rabbit",
      "geometry": {
        "kind": "model",
        "asset": { "local_path": "../../KzSUnity/Assets/3D/Characters/ch_KnifeRabbit/KnifeUsagi.fbx",
                   "remote_url": "", "sha256": "", "size_bytes": 0, "fetch_policy": "cache_first", "headers": [] },
        "rive_artboard": "", "text_default": "",
        "mesh": "none", "model": "handle:5", "shader": "none", "generic_handle": 0
      },
      "materials": [
        { "slot": "body", "material": "handle:12", "resource": { "local_path": "materials/body.mat.json" } }
      ],
      "textures": [
        { "slot": "diffuse", "texture": "none",
          "resource": { "local_path": "../../KzSUnity/Assets/3D/Characters/ch_KnifeRabbit/T_Knifeusagi_Albedo.png" } }
      ],
      "flags": { "default_flags": 2, "layer": 1, "pool_hint": "dynamic", "initial_lod": 0 },
      "animation_default": { "kind": "clip", "name": "Idle", "loop": true, "speed": 1 },
      "shader_key_override": 0
    })";
    VisusDesc d;
    std::string err;
    std::vector<std::string> warnings;
    PT_ASSERT(from_visus_json(v1, d, &err, &warnings), "v1 parse succeeds");
    PT_ASSERT(has_warning(warnings, "v1 converted"), "warns v1 converted");
    PT_ASSERT(has_warning(warnings, "handle dropped"), "warns about dropped handle");
    PT_ASSERT(d.name == "knife_rabbit" && d.kind == VisusKind::MODEL, "identity + kind");
    PT_ASSERT(d.asset.find("KnifeUsagi.fbx") != std::string::npos, "asset from local_path");
    PT_ASSERT_OP(d.parts.size(), ==, size_t{1}, "materials[] -> parts[]");
    PT_ASSERT(d.parts[0].part == "body" && d.parts[0].shader.is_default_builtin(), "slot -> part, builtin:pbr");
    PT_ASSERT(d.parts[0].metadata.get_string("material").value_or("") == "materials/body.mat.json", "material path kept");
    PT_ASSERT(d.metadata.get_string("texture.diffuse").value_or("").find("T_Knifeusagi_Albedo.png") != std::string::npos,
              "textures[] -> texture.<slot>");
    PT_ASSERT(d.metadata.get_number(visus_keys::kRenderFlags).value_or(0) == 2.0, "flags -> render.flags");
    PT_ASSERT(d.metadata.get_number(visus_keys::kRenderLayer).value_or(0) == 1.0, "layer -> render.layer");
    PT_ASSERT(d.metadata.get_string(visus_keys::kRenderPool).value_or("") == "dynamic", "pool_hint -> render.pool");
    PT_ASSERT(d.metadata.get_string(visus_keys::kAnimationDefault).value_or("") == "Idle", "animation name");
    PT_ASSERT(d.metadata.get_string(visus_keys::kAnimationKind).value_or("") == "clip", "animation kind");
    PT_ASSERT(d.metadata.get_bool(visus_keys::kAnimationLoop).value_or(false), "animation loop");
    PT_ASSERT(!d.metadata.has(visus_keys::kShaderKeyOverride), "zero key_override not emitted");

    const std::string out = to_visus_json(d);
    PT_ASSERT(out.find("\"version\": 2") != std::string::npos, "migrated emit is v2");
    PT_ASSERT(out.find("handle:") == std::string::npos, "no handles after migration");
}

// v1 CUSTOM kind (KS kuzu_custom_demo 相当) -> metadata["shader"] STAGES。
void test_v1_conversion_custom() {
    const char* v1 = R"({
      "version": 1, "name": "kuzu_custom_demo",
      "geometry": {
        "kind": "custom",
        "asset": { "local_path": "" },
        "shader_stages": {
          "vert": { "local_path": "../../shaders/custom_demo.vert.spv" },
          "frag": { "local_path": "../../shaders/custom_demo.frag.spv", "remote_url": "endpoint-marker" },
          "comp": { "local_path": "" },
          "vertex_layout": { "stride": 0, "attributes": [] }
        }
      },
      "shader_key_override": 7
    })";
    VisusDesc d;
    std::vector<std::string> warnings;
    PT_ASSERT(from_visus_json(v1, d, nullptr, &warnings), "v1 custom parse");
    PT_ASSERT(d.kind == VisusKind::CUSTOM, "custom kind");
    VisusShaderRef r;
    const VisusValue* sv = d.metadata.find(visus_keys::kShader);
    PT_ASSERT(sv && visus_shader_ref_from_value(*sv, r) &&
              r.vert.find("custom_demo.vert.spv") != std::string::npos &&
              r.frag.find("custom_demo.frag.spv") != std::string::npos && r.comp.empty(),
              "shader_stages -> metadata[shader] STAGES");
    PT_ASSERT(!d.metadata.has(visus_keys::kShaderVertexLayout), "empty vertex_layout not carried");
    PT_ASSERT(d.metadata.get_number(visus_keys::kShaderKeyOverride).value_or(0) == 7.0, "key override kept");
    PT_ASSERT(has_warning(warnings, "remote_url dropped"), "warns remote_url dropped");
    PT_ASSERT(!has_warning(warnings, "endpoint-marker"), "warning does not leak remote URL");
}

void test_v1_conversion_non_model_material() {
    const char* v1 = R"({
      "version": 1, "name": "quad", "geometry": { "kind": "primitive" },
      "materials": [
        { "slot": "surface", "resource": { "local_path": "materials/quad.mat.json" } },
        { "slot": "", "resource": { "local_path": "materials/default.mat.json" } }
      ]
    })";
    VisusDesc d;
    PT_ASSERT(from_visus_json(v1, d), "v1 primitive parses");
    PT_ASSERT(d.parts.empty(), "non-model material does not create model-only parts");
    PT_ASSERT(d.metadata.get_string("material.surface").value_or("") == "materials/quad.mat.json",
              "named material retained in metadata");
    PT_ASSERT(d.metadata.get_string(visus_keys::kMaterial).value_or("") == "materials/default.mat.json",
              "default material retained in metadata");
}

// `version` を落とした v1 文書は、 v2 として黙って読まれる (= name 以外を
// 全部捨てる) のではなく v1 変換に回る。
void test_version_less_v1_is_not_read_as_v2() {
    const char* v1 = R"({
      "name": "legacy",
      "geometry": { "kind": "model", "asset": { "local_path": "models/legacy.fbx" } },
      "textures": [ { "slot": "diffuse", "resource": { "local_path": "tex/legacy.png" } } ],
      "flags": { "default_flags": 2, "layer": 1 }
    })";
    VisusDesc d;
    std::vector<std::string> warnings;
    PT_ASSERT(from_visus_json(v1, d, nullptr, &warnings), "version-less v1 parses");
    PT_ASSERT(has_warning(warnings, "visus version missing"), "missing version is observable");
    PT_ASSERT(has_warning(warnings, "v1 converted"), "routed through the v1 converter");
    PT_ASSERT(d.kind == VisusKind::MODEL && d.asset == "models/legacy.fbx", "v1 geometry recovered");
    PT_ASSERT(d.metadata.get_string("texture.diffuse").value_or("") == "tex/legacy.png",
              "v1 textures recovered");
    PT_ASSERT(d.metadata.get_number(visus_keys::kRenderLayer).value_or(-1) == 1.0,
              "v1 flags recovered");

    // v2 側は誤検出しない: v1 固有 key が無ければ version 無しでも v2 のまま。
    VisusDesc v2;
    std::vector<std::string> v2_warnings;
    PT_ASSERT(from_visus_json(R"({"name":"x","kind":"rive","asset":"a.riv"})", v2, nullptr,
                              &v2_warnings),
              "version-less v2 parses");
    PT_ASSERT(v2_warnings.empty(), "version-less v2 is not misread as v1");
    PT_ASSERT(v2.kind == VisusKind::RIVE && v2.asset == "a.riv", "v2 fields read");
}

void test_syntax_error() {
    VisusDesc d;
    std::string err;
    PT_ASSERT(!from_visus_json("{\"name\": ", d, &err), "truncated JSON fails");
    PT_ASSERT(!err.empty(), "error message set");
    PT_ASSERT(!from_visus_json("[1,2]", d, &err), "non-object root fails");
    PT_ASSERT(!from_visus_json("{} trailing", d, &err), "trailing garbage fails");
    PT_ASSERT(!from_visus_json(R"({"version":1e})", d, &err), "missing exponent digits fail");
    PT_ASSERT(!from_visus_json(R"({"version":1e+})", d, &err), "missing signed exponent digits fail");
    PT_ASSERT(!from_visus_json(R"({"version":+1})", d, &err), "leading plus fails");
    PT_ASSERT(!from_visus_json(R"({"version":01})", d, &err), "leading zero fails");
    PT_ASSERT(!from_visus_json(R"({"version":1.})", d, &err), "missing fraction digits fail");
    PT_ASSERT(!from_visus_json(R"({"version":1e999})", d, &err), "overflowing number fails");
    PT_ASSERT(!from_visus_json(R"({"version":1.5})", d, &err), "ambiguous legacy version fails");
    PT_ASSERT(!from_visus_json(R"({"version":3,"name":"future","future_field":true})", d, &err),
              "future schema version fails instead of being read lossily as v2");
    PT_ASSERT(err.find("unsupported") != std::string::npos,
              "future schema version reports an unsupported-version error");
    PT_ASSERT(!from_visus_json(R"({"version":"1"})", d, &err), "non-numeric version fails");
    PT_ASSERT(!from_visus_json("{\"name\":\"line\nbreak\"}", d, &err),
              "unescaped control character fails");
    PT_ASSERT(!from_visus_json(R"({"name":"\uD800\u0041"})", d, &err),
              "mismatched surrogate pair fails");
    PT_ASSERT(!from_visus_json(R"({"name":"\uDC00"})", d, &err),
              "lone low surrogate fails");

    PT_ASSERT(from_visus_json(R"({"name":"\uD83D\uDE00"})", d, &err),
              "valid surrogate pair parses");
    PT_ASSERT(d.name == "\xF0\x9F\x98\x80", "surrogate pair emits valid UTF-8");
}

} // namespace

int main() {
    test_v2_round_trip();
    test_unknown_keys_preserved();
    test_invalid_parts_are_ignored();
    test_find_part_and_shader_ref();
    test_custom_kind_shader_in_metadata();
    test_v1_conversion_model();
    test_v1_conversion_custom();
    test_v1_conversion_non_model_material();
    test_version_less_v1_is_not_read_as_v2();
    test_syntax_error();
    return report("unit_visus_serializer_test");
}
