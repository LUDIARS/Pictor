#include "pictor/material/base_material_builder.h"
#include <algorithm>
#include <functional>

namespace pictor {

// ============================================================
// BaseMaterialBuilder — Texture setters
// ============================================================

BaseMaterialBuilder& BaseMaterialBuilder::albedo(TextureHandle tex) {
    desc_.albedo_texture = tex;
    return *this;
}

BaseMaterialBuilder& BaseMaterialBuilder::normal_map(TextureHandle tex) {
    desc_.normal_texture = tex;
    return *this;
}

BaseMaterialBuilder& BaseMaterialBuilder::metallic_map(TextureHandle tex) {
    desc_.metallic_texture = tex;
    return *this;
}

BaseMaterialBuilder& BaseMaterialBuilder::roughness_map(TextureHandle tex) {
    desc_.roughness_texture = tex;
    return *this;
}

BaseMaterialBuilder& BaseMaterialBuilder::ao_map(TextureHandle tex) {
    desc_.ao_texture = tex;
    return *this;
}

BaseMaterialBuilder& BaseMaterialBuilder::emissive_map(TextureHandle tex) {
    desc_.emissive_texture = tex;
    return *this;
}

// ============================================================
// BaseMaterialBuilder — Scalar / vector setters
// ============================================================

BaseMaterialBuilder& BaseMaterialBuilder::base_color(float r, float g, float b, float a) {
    desc_.base_color[0] = r;
    desc_.base_color[1] = g;
    desc_.base_color[2] = b;
    desc_.base_color[3] = a;
    return *this;
}

BaseMaterialBuilder& BaseMaterialBuilder::emissive_color(float r, float g, float b) {
    desc_.emissive[0] = r;
    desc_.emissive[1] = g;
    desc_.emissive[2] = b;
    return *this;
}

BaseMaterialBuilder& BaseMaterialBuilder::metallic_value(float v) {
    desc_.metallic = v;
    return *this;
}

BaseMaterialBuilder& BaseMaterialBuilder::roughness_value(float v) {
    desc_.roughness = v;
    return *this;
}

BaseMaterialBuilder& BaseMaterialBuilder::alpha_cutoff(float v) {
    desc_.alpha_cutoff = v;
    return *this;
}

BaseMaterialBuilder& BaseMaterialBuilder::normal_scale(float v) {
    desc_.normal_scale = v;
    return *this;
}

BaseMaterialBuilder& BaseMaterialBuilder::ao_strength(float v) {
    desc_.ao_strength = v;
    return *this;
}

// ============================================================
// BaseMaterialBuilder — Feature overrides
// ============================================================

BaseMaterialBuilder& BaseMaterialBuilder::enable_two_sided(bool enabled) {
    if (enabled)
        desc_.features |= MaterialFeature::TWO_SIDED;
    else
        desc_.features &= ~MaterialFeature::TWO_SIDED;
    return *this;
}

BaseMaterialBuilder& BaseMaterialBuilder::enable_vertex_color(bool enabled) {
    if (enabled)
        desc_.features |= MaterialFeature::VERTEX_COLOR;
    else
        desc_.features &= ~MaterialFeature::VERTEX_COLOR;
    return *this;
}

BaseMaterialBuilder& BaseMaterialBuilder::enable_alpha_test(bool enabled) {
    if (enabled)
        desc_.features |= MaterialFeature::ALPHA_TEST;
    else
        desc_.features &= ~MaterialFeature::ALPHA_TEST;
    return *this;
}

BaseMaterialBuilder& BaseMaterialBuilder::set_pass_features(PassType pass, uint32_t features) {
    pass_feature_overrides_[static_cast<size_t>(pass)] = features;
    return *this;
}

// ============================================================
// BaseMaterialBuilder — Feature detection
// ============================================================

uint32_t BaseMaterialBuilder::compute_features() const {
    uint32_t f = desc_.features;

    if (desc_.albedo_texture   != INVALID_TEXTURE) f |= MaterialFeature::ALBEDO_MAP;
    if (desc_.normal_texture   != INVALID_TEXTURE) f |= MaterialFeature::NORMAL_MAP;
    if (desc_.metallic_texture != INVALID_TEXTURE) f |= MaterialFeature::METALLIC_MAP;
    if (desc_.roughness_texture!= INVALID_TEXTURE) f |= MaterialFeature::ROUGHNESS_MAP;
    if (desc_.ao_texture       != INVALID_TEXTURE) f |= MaterialFeature::AO_MAP;
    if (desc_.emissive_texture != INVALID_TEXTURE) f |= MaterialFeature::EMISSIVE_MAP;
    if (desc_.alpha_cutoff > 0.0f)                  f |= MaterialFeature::ALPHA_TEST;

    return f;
}

// ============================================================
// BaseMaterialBuilder — Variant generation
// ============================================================

PassMaterialVariant BaseMaterialBuilder::build_variant(
    PassType pass, uint32_t full_features, uint32_t pass_requirement) const
{
    // Intersect: only keep features the pass actually needs AND the material provides
    uint32_t active = full_features & pass_requirement;

    PassMaterialVariant v;
    v.pass_type = pass;
    v.features  = active;

    // Texture bindings — only populate if the feature is active
    v.albedo_texture   = (active & MaterialFeature::ALBEDO_MAP)   ? desc_.albedo_texture   : INVALID_TEXTURE;
    v.normal_texture   = (active & MaterialFeature::NORMAL_MAP)   ? desc_.normal_texture   : INVALID_TEXTURE;
    v.metallic_texture = (active & MaterialFeature::METALLIC_MAP) ? desc_.metallic_texture : INVALID_TEXTURE;
    v.roughness_texture= (active & MaterialFeature::ROUGHNESS_MAP)? desc_.roughness_texture: INVALID_TEXTURE;
    v.ao_texture       = (active & MaterialFeature::AO_MAP)       ? desc_.ao_texture       : INVALID_TEXTURE;
    v.emissive_texture = (active & MaterialFeature::EMISSIVE_MAP) ? desc_.emissive_texture : INVALID_TEXTURE;

    // Scalar params — always copy base_color for passes that have albedo,
    // alpha_cutoff for passes with alpha test
    if (active & (MaterialFeature::ALBEDO_MAP | MaterialFeature::ALPHA_TEST)) {
        for (int i = 0; i < 4; ++i) v.base_color[i] = desc_.base_color[i];
    }
    if (active & MaterialFeature::ALPHA_TEST) {
        v.alpha_cutoff = desc_.alpha_cutoff;
    }
    if (active & MaterialFeature::EMISSIVE_MAP) {
        for (int i = 0; i < 3; ++i) v.emissive[i] = desc_.emissive[i];
    }
    if (active & MaterialFeature::NORMAL_MAP) {
        v.normal_scale = desc_.normal_scale;
    }
    if (active & MaterialFeature::AO_MAP) {
        v.ao_strength = desc_.ao_strength;
    }
    if (active & (MaterialFeature::METALLIC_MAP | MaterialFeature::ROUGHNESS_MAP)) {
        v.metallic  = desc_.metallic;
        v.roughness = desc_.roughness;
    }

    // Keys
    v.shader_key   = compute_shader_key(pass, active);
    v.material_key = compute_material_key(v);

    return v;
}

uint64_t BaseMaterialBuilder::compute_shader_key(PassType pass, uint32_t variant_features) {
    // Shader key layout:
    //   bits 63-56: PassType        (8 bits)
    //   bits 55-40: feature hash    (16 bits — shader permutation selector)
    //   bits 39-0 : reserved for future use
    //
    // Features that affect shader compilation are packed into the key so that
    // identical feature combinations share the same shader permutation.

    uint64_t key = 0;
    key |= static_cast<uint64_t>(pass) << 56;

    // Pack feature bits into the permutation selector.
    // Lower 10 bits of features map to unique shader variants.
    uint16_t perm = static_cast<uint16_t>(variant_features & 0x03FF);
    key |= static_cast<uint64_t>(perm) << 40;

    return key;
}

uint32_t BaseMaterialBuilder::compute_material_key(const PassMaterialVariant& variant) {
    // Material key is a hash of the active texture handles.
    // Objects sharing the same texture set can be batched together.
    uint32_t h = 0x811c9dc5u; // FNV-1a offset basis
    auto mix = [&](uint32_t val) {
        h ^= val;
        h *= 0x01000193u; // FNV-1a prime
    };

    mix(variant.albedo_texture);
    mix(variant.normal_texture);
    mix(variant.metallic_texture);
    mix(variant.roughness_texture);
    mix(variant.ao_texture);
    mix(variant.emissive_texture);

    return h;
}

// ============================================================
// BaseMaterialBuilder — build()
// ============================================================

BuiltMaterial BaseMaterialBuilder::build(MaterialHandle handle) const {
    BuiltMaterial result;
    result.handle = handle;
    result.desc   = desc_;

    uint32_t full_features = compute_features();
    result.desc.features = full_features;

    // Default pass requirements
    constexpr uint32_t default_requirements[] = {
        PassFeatureRequirement::DEPTH_ONLY,    // 0: DEPTH_ONLY
        PassFeatureRequirement::OPAQUE,         // 1: OPAQUE
        PassFeatureRequirement::TRANSPARENT,    // 2: TRANSPARENT
        PassFeatureRequirement::SHADOW,         // 3: SHADOW
        0,                                      // 4: POST_PROCESS (no material)
        0,                                      // 5: COMPUTE      (no material)
        PassFeatureRequirement::OPAQUE,         // 6: CUSTOM       (default to full)
    };

    for (size_t i = 0; i < BuiltMaterial::MAX_PASS_VARIANTS; ++i) {
        uint32_t req = pass_feature_overrides_[i]
                     ? pass_feature_overrides_[i]
                     : default_requirements[i];

        result.variants[i] = build_variant(
            static_cast<PassType>(i), full_features, req);
    }

    return result;
}

// ============================================================
// MaterialRegistry
// ============================================================

bool MaterialRegistry::register_material(BuiltMaterial&& mat) {
    // Grow vector if needed; handle is used as direct index.
    MaterialHandle h = mat.handle;
    if (h >= materials_.size()) {
        materials_.resize(static_cast<size_t>(h) + 1);
    }
    if (materials_[h].handle != INVALID_MATERIAL) {
        return false; // already registered
    }
    materials_[h] = std::move(mat);
    return true;
}

const BuiltMaterial* MaterialRegistry::get(MaterialHandle handle) const {
    if (handle >= materials_.size()) return nullptr;
    if (materials_[handle].handle == INVALID_MATERIAL) return nullptr;
    return &materials_[handle];
}

const PassMaterialVariant* MaterialRegistry::variant_for(
    MaterialHandle handle, PassType pass) const
{
    const BuiltMaterial* mat = get(handle);
    if (!mat) return nullptr;
    return &mat->variant_for(pass);
}

MaterialHandle MaterialRegistry::allocate_handle() {
    return next_handle_++;
}

} // namespace pictor
