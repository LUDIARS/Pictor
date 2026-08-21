/// シェーダーパッケージ (§2.5) — 実効列のマージ規則 / パッケージ JSON /
/// Visus 側 shader_packages の round-trip。

#include "pictor/visus/visus_package_serializer.h"
#include "pictor/visus/visus_serializer.h"
#include "pictor/visus/visus_shader_package.h"
#include "test_common.h"

#include <string>
#include <vector>

using namespace pictor;
using namespace pictor_test;

namespace {

VisusPackageRef ref(const char* name) {
    VisusPackageRef r;
    r.package = name;
    return r;
}

VisusPackageRef ref_param(const char* name, const char* key, double value) {
    VisusPackageRef r = ref(name);
    r.params.set(key, value);
    return r;
}

bool near_value(double a, double b) { return (a > b ? a - b : b - a) < 1e-9; }

std::string joined(const std::vector<VisusPackageRef>& list) {
    std::string out;
    for (const VisusPackageRef& r : list) {
        if (!out.empty()) out += ",";
        out += r.package;
    }
    return out;
}

// ---- §2.5.3 マージ規則 -------------------------------------------------------

void test_effective_order_and_append() {
    const std::vector<VisusPackageRef> visus_level = {ref("toon"), ref("outline")};
    const std::vector<VisusPackageRef> part_level  = {ref("skin_sss")};

    const std::vector<VisusPackageRef> eff = visus_effective_packages(visus_level, part_level);
    PT_ASSERT(joined(eff) == "toon,outline,skin_sss", "part level appends after visus level");

    PT_ASSERT(visus_effective_packages({}, {}).empty(), "no assignment -> empty");
    PT_ASSERT(joined(visus_effective_packages({}, part_level)) == "skin_sss",
              "part level alone");
}

void test_effective_overrides_in_place() {
    std::vector<VisusPackageRef> visus_level = {ref_param("toon", "rim_power", 2.0),
                                                ref("outline")};
    VisusPackageRef part_toon = ref_param("toon", "rim_power", 4.0);
    part_toon.params.set("tint", 0.5);
    part_toon.metadata.set("blend", "add");

    const std::vector<VisusPackageRef> eff =
        visus_effective_packages(visus_level, {part_toon});
    PT_ASSERT(joined(eff) == "toon,outline", "override keeps the original position");
    PT_ASSERT_OP(eff[0].params.get_number("rim_power").value_or(0.0), ==, 4.0,
                 "part level overrides the value");
    PT_ASSERT_OP(eff[0].params.get_number("tint").value_or(0.0), ==, 0.5,
                 "part level adds new keys");
    PT_ASSERT(eff[0].metadata.get_string("blend").value_or("") == "add",
              "metadata merges too");
}

void test_effective_disable() {
    VisusPackageRef off = ref("outline");
    off.enabled = false;
    const std::vector<VisusPackageRef> eff =
        visus_effective_packages({ref("toon"), ref("outline")}, {off});
    PT_ASSERT(joined(eff) == "toon", "enabled=false removes an inherited assignment");

    // 無効化しても後段で戻せる (同じ列内で先勝ち + マージ)。
    VisusPackageRef on = ref("outline");
    const std::vector<VisusPackageRef> back =
        visus_effective_packages({ref("toon"), ref("outline")}, {off, on});
    PT_ASSERT(joined(back) == "toon,outline", "re-enabling in the same list works");

    // 無効エントリ単独 (継承元なし) は列に残らない。
    PT_ASSERT(visus_effective_packages({}, {off}).empty(), "disabled-only assignment drops out");
}

void test_effective_params_merge_with_defaults() {
    VisusShaderPackage pkg;
    pkg.name = "toon";
    pkg.shader = VisusShaderRef::builtin("toon");
    pkg.params.set("rim_power", 2.0);
    pkg.params.set("tint", 1.0);
    pkg.metadata.set("blend", "alpha");

    const VisusPackageRef assign = ref_param("toon", "rim_power", 5.0);
    const VisusMetadata params = visus_package_effective_params(pkg, assign);
    PT_ASSERT_OP(params.get_number("rim_power").value_or(0.0), ==, 5.0, "override wins");
    PT_ASSERT_OP(params.get_number("tint").value_or(0.0), ==, 1.0, "default kept");
    PT_ASSERT_OP(params.size(), ==, size_t{2}, "no extra keys");

    const VisusMetadata meta = visus_package_effective_metadata(pkg, assign);
    PT_ASSERT(meta.get_string("blend").value_or("") == "alpha", "metadata default kept");
}

// ---- §2.5.1 パッケージ JSON ---------------------------------------------------

void test_package_json_round_trip() {
    VisusShaderPackage pkg;
    pkg.name   = "toon";
    pkg.shader = VisusShaderRef::stages("shaders/toon.vert.spv", "shaders/toon.frag.spv");
    pkg.params.set("rim_power", 2.5);
    pkg.params.set("texture.ramp", "../tex/ramp.png");
    pkg.metadata.set(visus_keys::kShaderKeyOverride, 3);

    const std::string json = to_shader_package_json(pkg);
    VisusShaderPackage back;
    std::string err;
    std::vector<std::string> warnings;
    PT_ASSERT(from_shader_package_json(json, back, &err, &warnings), "package json parses");
    PT_ASSERT(warnings.empty(), "no warnings on round-trip");
    PT_ASSERT(back.name == "toon", "name round-trips");
    PT_ASSERT(back.shader == pkg.shader, "shader round-trips");
    PT_ASSERT(back.params == pkg.params, "params round-trip");
    PT_ASSERT(back.metadata == pkg.metadata, "metadata round-trips");
}

void test_package_json_rejects_bad_input() {
    VisusShaderPackage pkg;
    std::string err;
    PT_ASSERT(!from_shader_package_json("[1,2]", pkg, &err), "array root rejected");
    PT_ASSERT(!from_shader_package_json("{\"version\": 99}", pkg, &err), "future version rejected");
    PT_ASSERT(!from_shader_package_json("{\"version\": \"1\"}", pkg, &err),
              "non-numeric version rejected");

    std::vector<std::string> warnings;
    PT_ASSERT(from_shader_package_json("{\"name\":\"p\",\"shader\":42}", pkg, &err, &warnings),
              "unknown shader form is not fatal");
    PT_ASSERT(pkg.shader.is_default_builtin(), "falls back to builtin:pbr");
    PT_ASSERT(!warnings.empty(), "and warns");
}

// ---- §2.5.2 Visus 側のアサイン ------------------------------------------------

void test_package_ref_value_forms() {
    VisusValue plain{std::string("toon")};
    VisusPackageRef r;
    PT_ASSERT(visus_package_ref_from_value(plain, r), "string form parses");
    PT_ASSERT(r.package == "toon" && r.enabled, "string form is an enabled plain assignment");
    PT_ASSERT(visus_package_ref_to_value(r).is_string(), "plain assignment emits as a string");

    VisusMetadata o;
    o.set("package", "outline");
    o.set("enabled", false);
    VisusMetadata params;
    params.set("width", 0.02);
    o.set("params", VisusValue(params));
    PT_ASSERT(visus_package_ref_from_value(VisusValue(o), r), "object form parses");
    PT_ASSERT(r.package == "outline" && !r.enabled, "enabled=false read");
    PT_ASSERT(near_value(r.params.get_number("width").value_or(0.0), 0.02), "params read");
    PT_ASSERT(visus_package_ref_to_value(r).is_object(), "non-plain assignment emits as object");

    std::vector<std::string> warnings;
    VisusMetadata nameless;
    nameless.set("params", VisusValue(params));
    PT_ASSERT(!visus_package_ref_from_value(VisusValue(nameless), r, &warnings),
              "entry without package rejected");
    PT_ASSERT(!visus_package_ref_from_value(VisusValue(2.0), r, &warnings), "number rejected");
    PT_ASSERT_OP(warnings.size(), ==, size_t{2}, "both rejections warn");
}

void test_visus_json_round_trip_with_packages() {
    VisusDesc d;
    d.name  = "kuzuha";
    d.kind  = VisusKind::MODEL;
    d.asset = "k.fbx";
    d.packages = {ref("toon"), ref_param("outline", "width", 0.02)};

    // base シェーダは従来通り typed フィールド。 パッケージはその後ろに重なる。
    VisusPart face;
    face.part   = "T_Face_bsc";
    face.shader = VisusShaderRef::builtin();
    VisusPackageRef off = ref("outline");
    off.enabled = false;
    face.packages = {off, ref_param("skin_sss", "thickness", 0.4)};
    d.parts = {face};

    const std::string json = to_visus_json(d);
    VisusDesc back;
    std::string err;
    std::vector<std::string> warnings;
    PT_ASSERT(from_visus_json(json, back, &err, &warnings), "visus json with packages parses");
    PT_ASSERT(warnings.empty(), "no warnings");
    PT_ASSERT_OP(back.packages.size(), ==, size_t{2}, "visus level assignments round-trip");
    PT_ASSERT(back.packages[0].package == "toon", "order preserved");
    PT_ASSERT(near_value(back.packages[1].params.get_number("width").value_or(0.0), 0.02),
              "assignment params round-trip");
    PT_ASSERT_OP(back.parts.size(), ==, size_t{1}, "part round-trips");
    PT_ASSERT_OP(back.parts[0].packages.size(), ==, size_t{2}, "part assignments round-trip");
    PT_ASSERT(!back.parts[0].packages[0].enabled, "enabled=false round-trips");

    const std::vector<VisusPackageRef> eff =
        visus_effective_packages(back.packages, back.parts[0].packages);
    PT_ASSERT(joined(eff) == "toon,skin_sss", "effective list after round-trip");
}

void test_visus_json_without_packages_is_unchanged() {
    VisusDesc d;
    d.name = "plain";
    d.kind = VisusKind::PRIMITIVE;
    d.asset = "cube.mesh";
    const std::string json = to_visus_json(d);
    PT_ASSERT(json.find("shader_packages") == std::string::npos,
              "no shader_packages key when nothing is assigned");

    VisusDesc back;
    PT_ASSERT(from_visus_json(json, back), "plain visus still parses");
    PT_ASSERT(back.packages.empty(), "no assignments");
}

} // namespace

int main() {
    test_effective_order_and_append();
    test_effective_overrides_in_place();
    test_effective_disable();
    test_effective_params_merge_with_defaults();
    test_package_json_round_trip();
    test_package_json_rejects_bad_input();
    test_package_ref_value_forms();
    test_visus_json_round_trip_with_packages();
    test_visus_json_without_packages_is_unchanged();
    return report("unit_visus_shader_package_test");
}
