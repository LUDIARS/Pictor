// Pictor — Shadow Sampling Utilities
// Provides CSM cascade selection, PCF soft shadows, PCSS contact-hardening,
// and self-shadow bias functions.
// Include via: #include "shadow.glsl" (requires GL_GOOGLE_include_directive)

#ifndef PICTOR_SHADOW_GLSL
#define PICTOR_SHADOW_GLSL

// ============================================================
// Shadow Map Uniforms (set = 2)
// ============================================================

layout(set = 2, binding = 0) uniform ShadowUniforms {
    mat4  shadowViewProj[4];     // per-cascade light-space VP
    vec4  cascadeSplitDepths;    // view-space Z split distances
    vec4  shadowParams;          // x = texelSize, y = cascadeCount,
                                 // z = filterMode, w = shadowStrength
    vec4  shadowBias;            // x = depthBias, y = normalBias,
                                 // z = slopeScaleBias, w = cascadeBlendWidth
    vec4  pcssParams;            // x = lightSize, y = minPenumbra,
                                 // z = maxPenumbra, w = blockerSearchRadius
};

layout(set = 2, binding = 1) uniform sampler2DArray shadowAtlas;

// ============================================================
// Constants
// ============================================================

const int SHADOW_FILTER_NONE = 0;
const int SHADOW_FILTER_PCF  = 1;
const int SHADOW_FILTER_PCSS = 2;

// Poisson disk samples for PCF / PCSS
const vec2 POISSON_DISK[16] = vec2[16](
    vec2(-0.94201624, -0.39906216),
    vec2( 0.94558609, -0.76890725),
    vec2(-0.09418410, -0.92938870),
    vec2( 0.34495938,  0.29387760),
    vec2(-0.91588581,  0.45771432),
    vec2(-0.81544232, -0.87912464),
    vec2(-0.38277543,  0.27676845),
    vec2( 0.97484398,  0.75648379),
    vec2( 0.44323325, -0.97511554),
    vec2( 0.53742981, -0.47373420),
    vec2(-0.26496911, -0.41893023),
    vec2( 0.79197514,  0.19090188),
    vec2(-0.24188840,  0.99706507),
    vec2(-0.81409955,  0.91437590),
    vec2( 0.19984126,  0.78641367),
    vec2( 0.14383161, -0.14100790)
);

// Rotated Poisson disk for temporal noise
vec2 rotatePoisson(vec2 sample, float angle) {
    float s = sin(angle);
    float c = cos(angle);
    return vec2(c * sample.x - s * sample.y,
                s * sample.x + c * sample.y);
}

// ============================================================
// Cascade Selection
// ============================================================

/// Select the cascade index for a given view-space depth.
/// Returns cascade index (0..cascadeCount-1).
int selectCascade(float viewDepth) {
    int cascadeCount = int(shadowParams.y);
    for (int i = 0; i < cascadeCount - 1; i++) {
        if (viewDepth < cascadeSplitDepths[i]) {
            return i;
        }
    }
    return cascadeCount - 1;
}

/// Compute cascade blend factor for smooth transitions.
/// Returns 0.0 at cascade center, 1.0 at cascade boundary.
float cascadeBlendFactor(float viewDepth, int cascade) {
    float blendWidth = shadowBias.w;
    if (blendWidth <= 0.0) return 0.0;

    float splitDist = cascadeSplitDepths[cascade];
    float fade = (splitDist - viewDepth) / blendWidth;
    return clamp(1.0 - fade, 0.0, 1.0);
}

// ============================================================
// Shadow Coordinate Computation
// ============================================================

/// Project a world-space position into shadow map UV + depth.
/// Returns vec3(u, v, depth) in [0,1] range.
vec3 worldToShadowCoord(vec3 worldPos, int cascade) {
    vec4 lightClip = shadowViewProj[cascade] * vec4(worldPos, 1.0);
    vec3 ndc = lightClip.xyz / lightClip.w;

    // Vulkan NDC: x,y in [-1,1], z in [0,1]
    return vec3(ndc.xy * 0.5 + 0.5, ndc.z);
}

// ============================================================
// Self-Shadow Bias
// ============================================================

/// Compute adaptive depth bias based on surface orientation relative to light.
/// Steeper angles get more bias to prevent shadow acne.
float computeSlopeBias(vec3 normal, vec3 lightDir) {
    float cosTheta = clamp(dot(normal, lightDir), 0.0, 1.0);
    float tanTheta = sqrt(1.0 - cosTheta * cosTheta) / max(cosTheta, 0.001);
    return shadowBias.x + shadowBias.z * min(tanTheta, 10.0);
}

/// Apply normal offset bias to the world position.
/// Shifts the sampling position along the surface normal.
vec3 applyNormalBias(vec3 worldPos, vec3 normal, vec3 lightDir) {
    float cosTheta = clamp(dot(normal, lightDir), 0.0, 1.0);
    float normalBiasScale = shadowBias.y * (1.0 - cosTheta);
    return worldPos + normal * normalBiasScale;
}

// ============================================================
// Hard Shadow (No Filtering)
// ============================================================

float sampleShadowHard(vec3 shadowCoord, int cascade) {
    float closestDepth = texture(shadowAtlas, vec3(shadowCoord.xy, float(cascade))).r;
    return (shadowCoord.z <= closestDepth) ? 1.0 : 0.0;
}

// ============================================================
// PCF (Percentage Closer Filtering)
// ============================================================

/// Standard PCF with configurable kernel.
/// Uses Poisson disk sampling for better quality than grid sampling.
float sampleShadowPCF(vec3 shadowCoord, int cascade, float filterRadius) {
    float texelSize = shadowParams.x;
    float shadow = 0.0;

    // Screen-space noise for temporal jitter
    float angle = fract(sin(dot(shadowCoord.xy, vec2(12.9898, 78.233))) * 43758.5453) * 6.2832;

    for (int i = 0; i < 16; i++) {
        vec2 offset = rotatePoisson(POISSON_DISK[i], angle) * filterRadius * texelSize;
        vec2 sampleUV = shadowCoord.xy + offset;

        float closestDepth = texture(shadowAtlas, vec3(sampleUV, float(cascade))).r;
        shadow += (shadowCoord.z <= closestDepth) ? 1.0 : 0.0;
    }

    return shadow / 16.0;
}

// ============================================================
// PCSS (Percentage Closer Soft Shadows)
// ============================================================

/// Blocker search: find average depth of occluders in a search region.
/// Returns average blocker depth, or -1 if no blockers found.
float findAverageBlockerDepth(vec3 shadowCoord, int cascade, float searchRadius) {
    float texelSize = shadowParams.x;
    float blockerSum = 0.0;
    int blockerCount = 0;

    float angle = fract(sin(dot(shadowCoord.xy, vec2(12.9898, 78.233))) * 43758.5453) * 6.2832;

    for (int i = 0; i < 16; i++) {
        vec2 offset = rotatePoisson(POISSON_DISK[i], angle) * searchRadius * texelSize;
        float sampleDepth = texture(shadowAtlas, vec3(shadowCoord.xy + offset, float(cascade))).r;

        if (sampleDepth < shadowCoord.z) {
            blockerSum += sampleDepth;
            blockerCount++;
        }
    }

    if (blockerCount == 0) return -1.0;
    return blockerSum / float(blockerCount);
}

/// PCSS: contact-hardening soft shadows.
/// Penumbra width scales with distance between blocker and receiver.
float sampleShadowPCSS(vec3 shadowCoord, int cascade) {
    float lightSize = pcssParams.x;
    float searchRadius = pcssParams.w;

    // Step 1: Blocker search
    float avgBlockerDepth = findAverageBlockerDepth(shadowCoord, cascade, searchRadius);

    // No blockers = fully lit
    if (avgBlockerDepth < 0.0) return 1.0;

    // Step 2: Estimate penumbra width
    float penumbraWidth = lightSize * (shadowCoord.z - avgBlockerDepth) / avgBlockerDepth;
    penumbraWidth = clamp(penumbraWidth, pcssParams.y, pcssParams.z);

    // Step 3: PCF with penumbra-scaled filter
    return sampleShadowPCF(shadowCoord, cascade, penumbraWidth);
}

// ============================================================
// Main Shadow Sampling Entry Point
// ============================================================

/// Sample shadow for a world-space position.
/// Handles cascade selection, bias, filtering, and cascade blending.
/// @param worldPos   fragment world-space position
/// @param viewDepth  fragment view-space depth (positive, toward camera)
/// @param normal     world-space surface normal
/// @param lightDir   normalized direction toward light
/// @return shadow factor: 1.0 = fully lit, 0.0 = fully shadowed
float sampleShadow(vec3 worldPos, float viewDepth, vec3 normal, vec3 lightDir) {
    int cascadeCount = int(shadowParams.y);
    float shadowStrength = shadowParams.w;

    // Outside shadow distance
    if (viewDepth > cascadeSplitDepths[cascadeCount - 1]) {
        return 1.0;
    }

    // Select primary cascade
    int cascade = selectCascade(viewDepth);

    // Apply normal bias
    vec3 biasedPos = applyNormalBias(worldPos, normal, lightDir);

    // Project to shadow coordinates
    vec3 shadowCoord = worldToShadowCoord(biasedPos, cascade);

    // Apply slope-scale depth bias
    shadowCoord.z -= computeSlopeBias(normal, lightDir);

    // Bounds check
    if (any(lessThan(shadowCoord.xy, vec2(0.0))) ||
        any(greaterThan(shadowCoord.xy, vec2(1.0)))) {
        return 1.0;
    }

    // Sample shadow based on filter mode
    float shadow;
    int filterMode = int(shadowParams.z);

    if (filterMode == SHADOW_FILTER_PCSS) {
        shadow = sampleShadowPCSS(shadowCoord, cascade);
    } else if (filterMode == SHADOW_FILTER_PCF) {
        shadow = sampleShadowPCF(shadowCoord, cascade, 1.5);
    } else {
        shadow = sampleShadowHard(shadowCoord, cascade);
    }

    // Cascade blending: blend with next cascade at boundaries
    float blendFactor = cascadeBlendFactor(viewDepth, cascade);
    if (blendFactor > 0.0 && cascade < cascadeCount - 1) {
        vec3 nextShadowCoord = worldToShadowCoord(biasedPos, cascade + 1);
        nextShadowCoord.z -= computeSlopeBias(normal, lightDir);

        float nextShadow;
        if (filterMode == SHADOW_FILTER_PCSS) {
            nextShadow = sampleShadowPCSS(nextShadowCoord, cascade + 1);
        } else if (filterMode == SHADOW_FILTER_PCF) {
            nextShadow = sampleShadowPCF(nextShadowCoord, cascade + 1, 1.5);
        } else {
            nextShadow = sampleShadowHard(nextShadowCoord, cascade + 1);
        }

        shadow = mix(shadow, nextShadow, blendFactor);
    }

    // Apply shadow strength (allows partial shadow for artistic control)
    return mix(1.0, shadow, shadowStrength);
}

// ============================================================
// Debug: Cascade Visualization
// ============================================================

/// Returns a debug color for the active cascade.
/// Useful for visualizing cascade boundaries.
vec3 debugCascadeColor(float viewDepth) {
    int cascade = selectCascade(viewDepth);
    const vec3 colors[4] = vec3[4](
        vec3(1.0, 0.2, 0.2),  // cascade 0: red
        vec3(0.2, 1.0, 0.2),  // cascade 1: green
        vec3(0.2, 0.2, 1.0),  // cascade 2: blue
        vec3(1.0, 1.0, 0.2)   // cascade 3: yellow
    );
    return colors[clamp(cascade, 0, 3)];
}

#endif // PICTOR_SHADOW_GLSL
