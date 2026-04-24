/// pictor_material_serializer_demo
///
/// Exercises `to_material_json()` / `from_material_json()` via a
/// round-trip: build a non-default MaterialDesc, serialize, parse
/// back, and assert every field survived. Prints the canonical JSON
/// so tool authors can see exactly what the C++ emitter produces.

#include "pictor/material/material_serializer.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>

using namespace pictor;

namespace {
bool near_eq(float a, float b, float eps = 1e-5f) {
    return std::fabs(a - b) <= eps;
}

int fail_count = 0;
void check(bool cond, const char* label) {
    if (!cond) {
        std::fprintf(stderr, "  ✗ %s\n", label);
        ++fail_count;
    } else {
        std::printf("  ✓ %s\n", label);
    }
}
} // namespace

int main() {
    std::printf("=== Pictor Material Serializer Demo ===\n\n");

    // ---- Construct a non-default descriptor -----------------
    MaterialDesc src;
    src.albedo_texture    = 42;
    src.normal_texture    = 7;
    src.metallic_texture  = INVALID_TEXTURE;
    src.roughness_texture = 13;
    src.ao_texture        = INVALID_TEXTURE;
    src.emissive_texture  = 99;
    src.base_color[0] = 0.8f; src.base_color[1] = 0.1f;
    src.base_color[2] = 0.15f; src.base_color[3] = 1.0f;
    src.emissive[0] = 0.0f; src.emissive[1] = 0.2f; src.emissive[2] = 0.7f;
    src.metallic       = 0.25f;
    src.roughness      = 0.75f;
    src.alpha_cutoff   = 0.33f;
    src.normal_scale   = 1.5f;
    src.ao_strength    = 0.9f;
    src.cast_shadow    = true;
    src.receive_shadow = false;
    src.features       = 0x0F;

    const std::string json = to_material_json(src, "demo_matte_red");
    std::printf("%s\n", json.c_str());

    // ---- Round-trip parse -----------------------------------
    MaterialDesc dst;
    std::string name, err;
    const bool ok = from_material_json(json, dst, &name, &err);
    check(ok, "from_material_json returned true");
    if (!ok) std::fprintf(stderr, "    error: %s\n", err.c_str());

    check(name == "demo_matte_red",          "name roundtrip");
    check(dst.albedo_texture    == 42,        "albedo handle");
    check(dst.normal_texture    == 7,         "normal handle");
    check(dst.metallic_texture  == INVALID_TEXTURE, "metallic none");
    check(dst.roughness_texture == 13,        "roughness handle");
    check(dst.ao_texture        == INVALID_TEXTURE, "ao none");
    check(dst.emissive_texture  == 99,        "emissive handle");

    check(near_eq(dst.base_color[0], 0.8f),   "base_color r");
    check(near_eq(dst.base_color[1], 0.1f),   "base_color g");
    check(near_eq(dst.base_color[2], 0.15f),  "base_color b");
    check(near_eq(dst.base_color[3], 1.0f),   "base_color a");
    check(near_eq(dst.emissive[0], 0.0f),     "emissive r");
    check(near_eq(dst.emissive[1], 0.2f),     "emissive g");
    check(near_eq(dst.emissive[2], 0.7f),     "emissive b");

    check(near_eq(dst.metallic,     0.25f),   "metallic");
    check(near_eq(dst.roughness,    0.75f),   "roughness");
    check(near_eq(dst.alpha_cutoff, 0.33f),   "alpha_cutoff");
    check(near_eq(dst.normal_scale, 1.5f),    "normal_scale");
    check(near_eq(dst.ao_strength,  0.9f),    "ao_strength");

    check(dst.cast_shadow    == true,         "cast_shadow");
    check(dst.receive_shadow == false,        "receive_shadow");
    check(dst.features       == 0x0Fu,        "features");

    // ---- Default round-trip ---------------------------------
    std::printf("\n-- default-desc roundtrip --\n");
    MaterialDesc defs;
    const std::string defs_json = to_material_json(defs);
    MaterialDesc defs_parsed;
    check(from_material_json(defs_json, defs_parsed), "default desc parses");
    check(defs_parsed.albedo_texture == INVALID_TEXTURE, "default albedo is none");
    check(near_eq(defs_parsed.roughness, 0.5f),          "default roughness 0.5");
    check(defs_parsed.cast_shadow == true,               "default cast_shadow");

    std::printf("\n");
    if (fail_count > 0) {
        std::fprintf(stderr, "✗ %d check(s) failed\n", fail_count);
        return EXIT_FAILURE;
    }
    std::printf("✓ All material serializer roundtrip checks passed.\n");
    return EXIT_SUCCESS;
}
