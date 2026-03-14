// Pictor — Ocean Wave Tessellation Evaluation Shader
// Generates wave displacement using layered Gerstner waves for
// realistic ocean surface animation.

#version 450

layout(quads, equal_spacing, ccw) in;

layout(set = 0, binding = 0) uniform SceneParams {
    mat4  view;
    mat4  projection;
    mat4  viewProjection;
    vec4  cameraPosition;
    float time;
    float maxTessLevel;
    float minTessLevel;
    float tessNearDist;
    float tessFarDist;
    float pad0, pad1, pad2;
};

layout(location = 0) in vec3 tcsPosition[];
layout(location = 1) in vec2 tcsTexCoord[];

layout(location = 0) out vec3 fragWorldPos;
layout(location = 1) out vec3 fragNormal;
layout(location = 2) out vec2 fragTexCoord;
layout(location = 3) out float fragFoam;

// ============================================================
// Gerstner Wave Parameters
// ============================================================
//
// Each wave: vec4(dirX, dirZ, steepness, wavelength)
// Using 6 overlapping waves for complex ocean surface

const int WAVE_COUNT = 6;
const vec4 waves[WAVE_COUNT] = vec4[](
    vec4( 0.8,  0.6,  0.35, 40.0),   // primary swell
    vec4( 0.3,  0.9,  0.25, 25.0),   // secondary swell
    vec4(-0.5,  0.7,  0.20, 15.0),   // cross swell
    vec4( 0.9, -0.3,  0.15, 10.0),   // wind chop 1
    vec4(-0.7, -0.5,  0.10,  6.0),   // wind chop 2
    vec4( 0.4, -0.8,  0.08,  4.0)    // ripple
);

const float waveAmplitudes[WAVE_COUNT] = float[](
    1.2, 0.8, 0.5, 0.3, 0.15, 0.08
);

const float waveSpeeds[WAVE_COUNT] = float[](
    1.0, 1.3, 0.9, 1.6, 2.0, 2.5
);

// ============================================================
// Gerstner Wave Evaluation
// ============================================================

struct WaveResult {
    vec3 position;
    vec3 normal;
    float foam;
};

WaveResult evaluateGerstnerWaves(vec2 xz) {
    vec3 pos = vec3(xz.x, 0.0, xz.y);
    vec3 tangent = vec3(1.0, 0.0, 0.0);
    vec3 binormal = vec3(0.0, 0.0, 1.0);
    float foamFactor = 0.0;

    for (int i = 0; i < WAVE_COUNT; ++i) {
        vec2 dir = normalize(waves[i].xy);
        float steepness = waves[i].z;
        float wavelength = waves[i].w;
        float amplitude = waveAmplitudes[i];
        float speed = waveSpeeds[i];

        float k = 2.0 * 3.14159265 / wavelength;
        float c = sqrt(9.81 / k);       // phase velocity (deep water dispersion)
        float f = k * (dot(dir, xz) - c * speed * time);
        float a = amplitude;
        float q = steepness / (k * a * float(WAVE_COUNT));  // Gerstner Q parameter

        float sinF = sin(f);
        float cosF = cos(f);

        // Gerstner displacement
        pos.x += q * a * dir.x * cosF;
        pos.z += q * a * dir.y * cosF;
        pos.y += a * sinF;

        // Partial derivatives for normal calculation
        float wa  = k * a;
        float s   = sinF;
        float co  = cosF;

        tangent.x -= q * dir.x * dir.x * wa * s;
        tangent.y += dir.x * wa * co;
        tangent.z -= q * dir.x * dir.y * wa * s;

        binormal.x -= q * dir.x * dir.y * wa * s;
        binormal.y += dir.y * wa * co;
        binormal.z -= q * dir.y * dir.y * wa * s;

        // Foam: accumulates at wave crests where curvature is high
        foamFactor += max(sinF, 0.0) * steepness;
    }

    WaveResult result;
    result.position = pos;
    result.normal = normalize(cross(binormal, tangent));
    result.foam = clamp(foamFactor - 0.3, 0.0, 1.0);
    return result;
}

void main() {
    // Bilinear interpolation of patch corners
    float u = gl_TessCoord.x;
    float v = gl_TessCoord.y;

    vec3 p0 = mix(tcsPosition[0], tcsPosition[1], u);
    vec3 p1 = mix(tcsPosition[3], tcsPosition[2], u);
    vec3 pos = mix(p0, p1, v);

    vec2 t0 = mix(tcsTexCoord[0], tcsTexCoord[1], u);
    vec2 t1 = mix(tcsTexCoord[3], tcsTexCoord[2], u);
    vec2 texCoord = mix(t0, t1, v);

    // Apply Gerstner wave displacement
    WaveResult wave = evaluateGerstnerWaves(pos.xz);

    fragWorldPos = wave.position;
    fragNormal = wave.normal;
    fragTexCoord = texCoord;
    fragFoam = wave.foam;

    gl_Position = viewProjection * vec4(wave.position, 1.0);
}
