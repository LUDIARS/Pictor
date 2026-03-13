#pragma once

#include "pictor/material/material_property.h"
#include <array>
#include <vector>

namespace pictor {

// ============================================================
// Built Material
// ============================================================
// Output of BaseMaterialBuilder::build().
// Holds the canonical MaterialDesc and per-pass variants.

struct BuiltMaterial {
    MaterialHandle handle = INVALID_MATERIAL;
    MaterialDesc   desc;

    // Indexed by PassType enum value for O(1) lookup.
    // Only DEPTH_ONLY..SHADOW (0..3) carry meaningful variants.
    // POST_PROCESS / COMPUTE / CUSTOM get a default (no-op) variant.
    static constexpr size_t MAX_PASS_VARIANTS = 7;
    std::array<PassMaterialVariant, MAX_PASS_VARIANTS> variants;

    const PassMaterialVariant& variant_for(PassType pass) const {
        return variants[static_cast<size_t>(pass)];
    }
};

// ============================================================
// Base Material Builder
// ============================================================
// Fluent builder that constructs a BuiltMaterial with per-pass variants.
//
// Usage:
//   auto mat = BaseMaterialBuilder()
//       .albedo(tex_albedo)
//       .normal_map(tex_normal)
//       .metallic(0.0f)
//       .roughness(0.8f)
//       .build(handle_allocator.next());
//
// Each variant is automatically stripped to only the features the
// target pass actually reads (see PassFeatureRequirement).

class BaseMaterialBuilder {
public:
    BaseMaterialBuilder() = default;

    // --- Texture setters (fluent) ---

    BaseMaterialBuilder& albedo(TextureHandle tex);
    BaseMaterialBuilder& normal_map(TextureHandle tex);
    BaseMaterialBuilder& metallic_map(TextureHandle tex);
    BaseMaterialBuilder& roughness_map(TextureHandle tex);
    BaseMaterialBuilder& ao_map(TextureHandle tex);
    BaseMaterialBuilder& emissive_map(TextureHandle tex);

    // --- Scalar / vector setters (fluent) ---

    BaseMaterialBuilder& base_color(float r, float g, float b, float a = 1.0f);
    BaseMaterialBuilder& emissive_color(float r, float g, float b);
    BaseMaterialBuilder& metallic_value(float v);
    BaseMaterialBuilder& roughness_value(float v);
    BaseMaterialBuilder& alpha_cutoff(float v);
    BaseMaterialBuilder& normal_scale(float v);
    BaseMaterialBuilder& ao_strength(float v);

    // --- Feature overrides ---

    BaseMaterialBuilder& enable_two_sided(bool enabled = true);
    BaseMaterialBuilder& enable_vertex_color(bool enabled = true);
    BaseMaterialBuilder& enable_alpha_test(bool enabled = true);

    // --- Custom pass feature override ---
    // Override the default PassFeatureRequirement for a specific pass.
    // For example, a custom GI pass that also needs normal maps.
    BaseMaterialBuilder& set_pass_features(PassType pass, uint32_t features);

    // --- Build ---

    /// Build the material and generate per-pass variants.
    /// `handle` is the MaterialHandle to assign.
    BuiltMaterial build(MaterialHandle handle) const;

    /// Access the in-progress descriptor (for inspection before build)
    const MaterialDesc& descriptor() const { return desc_; }

private:
    MaterialDesc desc_;

    // Per-pass feature overrides. 0 = use default.
    std::array<uint32_t, BuiltMaterial::MAX_PASS_VARIANTS> pass_feature_overrides_ = {};

    /// Compute automatic feature flags from the current descriptor
    uint32_t compute_features() const;

    /// Generate a PassMaterialVariant by intersecting full features with pass requirements
    PassMaterialVariant build_variant(PassType pass, uint32_t full_features,
                                      uint32_t pass_requirement) const;

    /// Compute shader key from active features for a pass
    static uint64_t compute_shader_key(PassType pass, uint32_t variant_features);

    /// Compute material key for batching (texture combination hash)
    static uint32_t compute_material_key(const PassMaterialVariant& variant);
};

// ============================================================
// Material Registry
// ============================================================
// Central storage for all BuiltMaterials. Provides O(1) lookup by handle
// and per-pass variant queries.

class MaterialRegistry {
public:
    MaterialRegistry() = default;

    /// Register a built material. Returns false if handle already exists.
    bool register_material(BuiltMaterial&& mat);

    /// Look up material by handle.
    const BuiltMaterial* get(MaterialHandle handle) const;

    /// Get the pass-specific variant for a material.
    /// Returns nullptr if material not found.
    const PassMaterialVariant* variant_for(MaterialHandle handle, PassType pass) const;

    /// Allocate the next available handle
    MaterialHandle allocate_handle();

    /// Number of registered materials
    size_t count() const { return materials_.size(); }

private:
    std::vector<BuiltMaterial> materials_;
    MaterialHandle next_handle_ = 0;
};

} // namespace pictor
