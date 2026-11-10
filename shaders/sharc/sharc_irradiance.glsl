// Pictor — SHaRC irradiance cache: shared definitions
// (include/pictor/gi/sharc_irradiance_types.h mirrors the layouts).
//
// The module's spatial hash, cell storage and accumulation (sharc_common.glsl)
// hold, per cell, the diffuse irradiance arriving there: M0 of the moments is
// the running mean, meta's sample count how many samples it stands for. A
// cell is (grid, level, facing): the facing (the dominant axis of the surface
// normal and its sign) keeps the two sides of a thin wall, and the floor and
// wall of a corner, in separate cells so light does not leak between them.
//
// Bindings 0-19 are sharc_common's; the irradiance passes add 30 and up.

#ifndef PICTOR_SHARC_IRRADIANCE_GLSL
#define PICTOR_SHARC_IRRADIANCE_GLSL

#include "sharc_common.glsl"

layout(std140, set = SHARC_SET, binding = 30) uniform SharcIrradianceFrame {
    mat4  sirInverseViewProjection;
    mat4  sirSunViewProjection;
    vec4  sirCamera;
    vec4  sirScreen;        // width, height, 1/width, 1/height
    vec4  sirSunDirection;  // xyz toward the sun, w 1 when there is a sun
    vec4  sirSunRadiance;   // rgb, w shadow bias
    vec4  sirSky;           // rgb, w light scale
    vec4  sirHorizon;
    vec4  sirGround;
    vec4  sirLimits;        // sample clamp, history samples, ray length, strength
    uvec4 sirCounts;        // light count, frame index, update block, sun shadow bound
};

const uint SHARC_IRRADIANCE_FACINGS = 6u;

// World position of the G-buffer pixel at `uv` (0..1, y down) with device depth `depth`.
vec3 sharcIrradianceWorld(vec2 uv, float depth) {
    vec4 clip = vec4(uv * 2.0 - 1.0, depth, 1.0);
    vec4 world = sirInverseViewProjection * clip;
    return world.xyz / world.w;
}

uint sharcIrradianceFacing(vec3 n) {
    vec3 a = abs(n);
    uint axis = (a.x > a.y && a.x > a.z) ? 0u : (a.y > a.z ? 1u : 2u);
    return axis * 2u + (n[axis] < 0.0 ? 1u : 0u);
}

// The level a point gets from its distance to the camera.
uint sharcIrradianceLevel(vec3 world) {
    return sharcLevelFromDistance(length(world - sirCamera.xyz));
}

// The hash's level field carries the facing above the true level (which
// sharcCellSize still reads from the low part).
uint sharcIrradianceKeyLevel(uint level, vec3 n) {
    return level + sharcLevelCount * sharcIrradianceFacing(n);
}

ivec3 sharcIrradianceGrid(vec3 world, uint level) {
    return sharcGridCoord(world, level);
}

// Irradiance cached at `world` facing `n`; false when nothing is known there yet.
bool sharcIrradianceLookup(vec3 world, vec3 n, out vec3 irradiance) {
    uint level = sharcIrradianceLevel(world);
    uint slot = sharcFindSlot(sharcIrradianceGrid(world, level), sharcIrradianceKeyLevel(level, n));
    irradiance = vec3(0.0);
    if (slot == SHARC_SLOT_INVALID) return false;
    uint samples, last, cellLevel;
    float mean, variance;
    sharcLoadMeta(slot, samples, last, mean, variance, cellLevel);
    if (samples == 0u) return false;
    vec3 m1Dir, m1Mag;
    float confidence;
    sharcLoadMoments(slot, irradiance, m1Dir, m1Mag, confidence);
    return true;
}

#endif  // PICTOR_SHARC_IRRADIANCE_GLSL
