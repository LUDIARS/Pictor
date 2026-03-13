#pragma once

#include "pictor/core/types.h"
#include <cstdint>

namespace pictor {

// ============================================================
// Material Feature Flags
// ============================================================
// Each bit indicates a feature the material uses.
// Pass-specific variants strip features not needed for that pass,
// reducing texture bindings and shader permutations.

namespace MaterialFeature {
    constexpr uint32_t NONE            = 0;
    constexpr uint32_t ALBEDO_MAP      = 1 << 0;
    constexpr uint32_t NORMAL_MAP      = 1 << 1;
    constexpr uint32_t METALLIC_MAP    = 1 << 2;
    constexpr uint32_t ROUGHNESS_MAP   = 1 << 3;
    constexpr uint32_t AO_MAP          = 1 << 4;
    constexpr uint32_t EMISSIVE_MAP    = 1 << 5;
    constexpr uint32_t ALPHA_TEST      = 1 << 6;   // alpha cutoff enabled
    constexpr uint32_t TWO_SIDED       = 1 << 7;
    constexpr uint32_t VERTEX_COLOR    = 1 << 8;
    constexpr uint32_t METALLIC_ROUGHNESS_PACKED = 1 << 9; // single ORM map
    constexpr uint32_t CAST_SHADOW    = 1 << 10;  // material casts shadows
    constexpr uint32_t RECEIVE_SHADOW = 1 << 11;  // material receives shadows

    // Full PBR mask
    constexpr uint32_t PBR_FULL = ALBEDO_MAP | NORMAL_MAP | METALLIC_MAP
                                | ROUGHNESS_MAP | AO_MAP | EMISSIVE_MAP;

    // Default shadow flags (most materials cast and receive)
    constexpr uint32_t SHADOW_DEFAULT = CAST_SHADOW | RECEIVE_SHADOW;
}

// ============================================================
// Material Descriptor
// ============================================================
// Full description of a material before pass-specific stripping.

struct MaterialDesc {
    // --- Texture bindings ---
    TextureHandle albedo_texture     = INVALID_TEXTURE;
    TextureHandle normal_texture     = INVALID_TEXTURE;
    TextureHandle metallic_texture   = INVALID_TEXTURE;
    TextureHandle roughness_texture  = INVALID_TEXTURE;
    TextureHandle ao_texture         = INVALID_TEXTURE;
    TextureHandle emissive_texture   = INVALID_TEXTURE;

    // --- Scalar / vector params ---
    float base_color[4]  = {1.0f, 1.0f, 1.0f, 1.0f};
    float emissive[3]    = {0.0f, 0.0f, 0.0f};
    float metallic       = 0.0f;
    float roughness      = 0.5f;
    float alpha_cutoff   = 0.0f;   // 0 = no alpha test
    float normal_scale   = 1.0f;
    float ao_strength    = 1.0f;

    // --- Shadow flags ---
    bool     cast_shadow   = true;   // participates in shadow pass
    bool     receive_shadow = true;  // samples shadow map in fragment shader

    // --- Feature flags (auto-detected from bindings, can be overridden) ---
    uint32_t features    = MaterialFeature::NONE;
};

// ============================================================
// Pass Material Variant
// ============================================================
// Stripped-down view for a specific render pass.
// Carries only the features and bindings that pass actually reads.

struct PassMaterialVariant {
    PassType      pass_type    = PassType::OPAQUE;
    uint32_t      features     = MaterialFeature::NONE;  // features active in this pass
    uint64_t      shader_key   = 0;   // pass-specific shader permutation key
    uint32_t      material_key = 0;   // batching key for this variant

    // Texture bindings (INVALID_TEXTURE if not used by this pass)
    TextureHandle albedo_texture   = INVALID_TEXTURE;
    TextureHandle normal_texture   = INVALID_TEXTURE;
    TextureHandle metallic_texture = INVALID_TEXTURE;
    TextureHandle roughness_texture = INVALID_TEXTURE;
    TextureHandle ao_texture       = INVALID_TEXTURE;
    TextureHandle emissive_texture = INVALID_TEXTURE;

    // Scalar params (only populated if relevant to the pass)
    float alpha_cutoff  = 0.0f;
    float base_color[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    float emissive[3]   = {0.0f, 0.0f, 0.0f};
    float metallic      = 0.0f;
    float roughness     = 0.5f;
    float normal_scale  = 1.0f;
    float ao_strength   = 1.0f;
};

// ============================================================
// Pass Feature Requirements
// ============================================================
// Defines which material features each pass type actually reads.
// Used by BaseMaterialBuilder to strip unused bindings.

namespace PassFeatureRequirement {
    // Shadow: depth writing from light's perspective.
    // Needs ALBEDO_MAP for alpha-tested materials, TWO_SIDED for backface,
    // CAST_SHADOW to filter materials that actually cast shadows.
    constexpr uint32_t SHADOW     = MaterialFeature::ALBEDO_MAP
                                  | MaterialFeature::ALPHA_TEST
                                  | MaterialFeature::TWO_SIDED
                                  | MaterialFeature::CAST_SHADOW;

    constexpr uint32_t DEPTH_ONLY = MaterialFeature::ALPHA_TEST
                                  | MaterialFeature::TWO_SIDED;

    // GI: diffuse bounce needs albedo + emissive. No specular detail.
    constexpr uint32_t GI         = MaterialFeature::ALBEDO_MAP
                                  | MaterialFeature::EMISSIVE_MAP
                                  | MaterialFeature::ALPHA_TEST
                                  | MaterialFeature::TWO_SIDED
                                  | MaterialFeature::VERTEX_COLOR;

    // Forward opaque: everything.
    constexpr uint32_t OPAQUE     = 0xFFFFFFFF;

    // Transparent: everything (same as opaque but sorted differently).
    constexpr uint32_t TRANSPARENT = 0xFFFFFFFF;
}

} // namespace pictor
