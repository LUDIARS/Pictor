/**
 * Material JSON schema — mirrors `pictor::MaterialDesc` and the
 * `to_material_json()` C++ emitter. Kept in a single module so
 * the UI file stays focused on DOM wiring.
 */

export const MATERIAL_FEATURE = {
  ALBEDO_MAP:      1 <<  0,
  NORMAL_MAP:      1 <<  1,
  METALLIC_MAP:    1 <<  2,
  ROUGHNESS_MAP:   1 <<  3,
  AO_MAP:          1 <<  4,
  EMISSIVE_MAP:    1 <<  5,
  ALPHA_TEST:      1 <<  6,
  TWO_SIDED:       1 <<  7,
  VERTEX_COLOR:    1 <<  8,
  METALLIC_ROUGHNESS_PACKED: 1 << 9,
  CAST_SHADOW:     1 << 10,
  RECEIVE_SHADOW:  1 << 11,
};

export const CATALOG = {
  textures: [
    { key: "albedo",    label: "albedo"    },
    { key: "normal",    label: "normal"    },
    { key: "metallic",  label: "metallic"  },
    { key: "roughness", label: "roughness" },
    { key: "ao",        label: "ao"        },
    { key: "emissive",  label: "emissive"  },
  ],
  scalars: [
    { key: "metallic",     min: 0, max: 1, step: 0.01 },
    { key: "roughness",    min: 0, max: 1, step: 0.01 },
    { key: "alpha_cutoff", min: 0, max: 1, step: 0.01 },
    { key: "normal_scale", min: 0, max: 4, step: 0.01 },
    { key: "ao_strength",  min: 0, max: 1, step: 0.01 },
  ],
};

/** Fresh default material (matches C++ MaterialDesc defaults). */
export function defaultMaterial() {
  return {
    textures: {
      albedo:    "none",
      normal:    "none",
      metallic:  "none",
      roughness: "none",
      ao:        "none",
      emissive:  "none",
    },
    params: {
      base_color:   [1, 1, 1, 1],
      emissive:     [0, 0, 0],
      metallic:     0,
      roughness:    0.5,
      alpha_cutoff: 0,
      normal_scale: 1,
      ao_strength:  1,
    },
    flags: {
      cast_shadow:    true,
      receive_shadow: true,
      features:       0,
    },
  };
}

/** Canonical pretty-printed JSON, matching the C++ emitter. */
export function serialize(desc, name = "") {
  const j = {
    version: 1,
    ...(name ? { name } : {}),
    textures: { ...desc.textures },
    params: {
      base_color: [...desc.params.base_color],
      emissive:   [...desc.params.emissive],
      metallic:     desc.params.metallic,
      roughness:    desc.params.roughness,
      alpha_cutoff: desc.params.alpha_cutoff,
      normal_scale: desc.params.normal_scale,
      ao_strength:  desc.params.ao_strength,
    },
    flags: {
      cast_shadow:    desc.flags.cast_shadow,
      receive_shadow: desc.flags.receive_shadow,
      features:       desc.flags.features | 0,
    },
  };
  return JSON.stringify(j, null, 2);
}

/** Parse canonical JSON back into { name, desc }. Missing keys
 *  fall back to defaults; unknown keys are ignored. */
export function deserialize(text) {
  const raw = JSON.parse(text);
  if (raw == null || typeof raw !== "object") {
    throw new Error("not an object");
  }
  const desc = defaultMaterial();
  const name = typeof raw.name === "string" ? raw.name : "";

  if (raw.textures && typeof raw.textures === "object") {
    for (const k of Object.keys(desc.textures)) {
      const v = raw.textures[k];
      if (typeof v === "string") desc.textures[k] = v;
    }
  }
  if (raw.params && typeof raw.params === "object") {
    const p = raw.params;
    if (Array.isArray(p.base_color)) {
      for (let i = 0; i < 4 && i < p.base_color.length; ++i) {
        const n = Number(p.base_color[i]);
        if (Number.isFinite(n)) desc.params.base_color[i] = n;
      }
    }
    if (Array.isArray(p.emissive)) {
      for (let i = 0; i < 3 && i < p.emissive.length; ++i) {
        const n = Number(p.emissive[i]);
        if (Number.isFinite(n)) desc.params.emissive[i] = n;
      }
    }
    for (const k of ["metallic","roughness","alpha_cutoff","normal_scale","ao_strength"]) {
      const n = Number(p[k]);
      if (Number.isFinite(n)) desc.params[k] = n;
    }
  }
  if (raw.flags && typeof raw.flags === "object") {
    const f = raw.flags;
    if (typeof f.cast_shadow    === "boolean") desc.flags.cast_shadow    = f.cast_shadow;
    if (typeof f.receive_shadow === "boolean") desc.flags.receive_shadow = f.receive_shadow;
    const fe = Number(f.features);
    if (Number.isFinite(fe)) desc.flags.features = fe | 0;
  }
  return { name, desc };
}
