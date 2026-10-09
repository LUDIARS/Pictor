// Packs lighting into the float layout of pictor_web_set_lighting:
//   sky rgb, ground rgb, hemisphere intensity, light count,
//   then per light (MAX_DIRECTIONAL_LIGHTS slots): direction xyz, colour rgb, intensity.

export const MAX_DIRECTIONAL_LIGHTS = 2;
const FLOATS_PER_LIGHT = 7;
export const LIGHTING_FLOATS = 8 + MAX_DIRECTIONAL_LIGHTS * FLOATS_PER_LIGHT;

/**
 * @typedef {object} DirectionalLight
 * @property {[number, number, number]} direction from the surface towards the light
 * @property {[number, number, number]} color sRGB 0..1
 * @property {number} intensity
 */

/**
 * @typedef {object} Lighting
 * @property {[number, number, number]} sky sRGB 0..1
 * @property {[number, number, number]} ground sRGB 0..1
 * @property {number} hemisphereIntensity
 * @property {DirectionalLight[]} directional at most MAX_DIRECTIONAL_LIGHTS
 */

function rgbOrVector(value, name) {
  if (!Array.isArray(value) && !ArrayBuffer.isView(value)) throw new TypeError(`${name} must be 3 numbers`);
  if (value.length !== 3) throw new TypeError(`${name} must be 3 numbers`);
  return value;
}

/** @param {Lighting} lighting @returns {Float32Array} */
export function packLighting(lighting) {
  const directional = lighting.directional ?? [];
  if (directional.length > MAX_DIRECTIONAL_LIGHTS) {
    throw new RangeError(`at most ${MAX_DIRECTIONAL_LIGHTS} directional lights are supported`);
  }
  const out = new Float32Array(LIGHTING_FLOATS);
  out.set(rgbOrVector(lighting.sky, "sky"), 0);
  out.set(rgbOrVector(lighting.ground, "ground"), 3);
  out[6] = lighting.hemisphereIntensity;
  out[7] = directional.length;
  directional.forEach((light, index) => {
    const base = 8 + index * FLOATS_PER_LIGHT;
    out.set(rgbOrVector(light.direction, `directional[${index}].direction`), base);
    out.set(rgbOrVector(light.color, `directional[${index}].color`), base + 3);
    out[base + 6] = light.intensity;
  });
  return out;
}

/** 0xRRGGBB → sRGB components 0..1 (for hosts that write colours as hex). */
export function hexToRgb(hex) {
  if (!Number.isInteger(hex) || hex < 0 || hex > 0xffffff) throw new RangeError("colour must be 0x000000..0xffffff");
  return [((hex >> 16) & 0xff) / 255, ((hex >> 8) & 0xff) / 255, (hex & 0xff) / 255];
}
