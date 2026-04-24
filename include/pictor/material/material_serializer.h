#pragma once

#include "pictor/material/material_property.h"
#include <string>

namespace pictor {

// ============================================================
// Material Serializer
// ============================================================
// Canonical MaterialDesc ↔ JSON round-trip, kept outside
// BaseMaterialBuilder so the builder stays a pure fluent API.
//
// JSON layout (stable, versioned):
//   {
//     "version":         1,
//     "name":            "optional human-readable label",
//     "textures": {
//       "albedo":       "handle:<n>|0",
//       "normal":       "handle:<n>|0",
//       "metallic":     ...,
//       "roughness":    ...,
//       "ao":           ...,
//       "emissive":     ...
//     },
//     "params": {
//       "base_color":   [r, g, b, a],
//       "emissive":     [r, g, b],
//       "metallic":     0.0,
//       "roughness":    0.5,
//       "alpha_cutoff": 0.0,
//       "normal_scale": 1.0,
//       "ao_strength":  1.0
//     },
//     "flags": {
//       "cast_shadow":   true,
//       "receive_shadow": true,
//       "features":     <uint32_t>   // bitwise MaterialFeature::*
//     }
//   }
//
// Textures are emitted as opaque handle strings ("handle:<N>")
// so editors can display / remap them without knowing the
// runtime handle allocation scheme. `INVALID_TEXTURE`
// (std::numeric_limits<uint32_t>::max()) serializes to "none".
//
// Parsing is permissive: unknown keys are ignored and missing
// keys fall back to MaterialDesc defaults. Only malformed JSON
// (syntax error) returns false.

/// Serialize `desc` into a pretty-printed JSON string.
/// `name` is optional and embedded as the "name" field.
std::string to_material_json(const MaterialDesc& desc,
                             const std::string& name = {});

/// Parse JSON into `out`. Returns false on syntax error; `error`
/// (if non-null) receives a short human-readable message. On
/// partial parse the output is still safe (filled with defaults
/// for missing fields).
bool from_material_json(const std::string& json,
                        MaterialDesc&      out,
                        std::string*       name_out = nullptr,
                        std::string*       error    = nullptr);

} // namespace pictor
