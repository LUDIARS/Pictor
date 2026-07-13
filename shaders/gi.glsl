// Pictor — GI probe grid sampling (fragment include)
// probe grid の L2 SH を worldPos で trilinear 補間し、 法線方向の間接光
// irradiance を返す。 バッファは GIGpuExecutor が所有する実体を結線する:
//   set 2, binding 2 = GIProbeParams UBO   (executor->params_buffer())
//   set 2, binding 3 = ProbeIrradiance SSBO (executor->probe_sh_buffer())
// レイアウトは gi_probe_sample.comp / pictor::gi_sh.h と同一
// (係数 × vec4、 SH は間接光のみ — 直接光はマテリアルが解析評価する)。
//
// pbr_gi.frag (PICTOR_GI define) からのみ include される。 set/binding は
// shadow.glsl (set 2, binding 0/1) の続き。

layout(set = 2, binding = 2) uniform GIProbeParams {
    vec4  giGridOrigin;        // xyz = probe grid 原点
    vec4  giGridSpacing;       // xyz = probe 間隔
    uvec4 giGridDimensions;    // xyz = probe 数, w = 総数
    uint  giObjectCount;
    uint  giProbeCount;
    float giIntensity;         // 0 = GI 無効 (バッファ結線は維持)
    float giMaxProbeDistance;
};

layout(std430, set = 2, binding = 3) readonly buffer GIProbeIrradiance {
    vec4 gi_probe_sh[];        // [probeIdx * 9 + coeff]
};

// 8 probe の trilinear 補間 SH を得る (grid 外はクランプ)。
void giInterpolateProbes(vec3 worldPos, out vec4 sh[9]) {
    for (int i = 0; i < 9; i++) sh[i] = vec4(0.0);
    if (giProbeCount == 0u) return;

    vec3 local = (worldPos - giGridOrigin.xyz) / max(giGridSpacing.xyz,
                                                     vec3(1e-6));
    vec3 maxIdx = vec3(giGridDimensions.xyz) - 1.0;
    local = clamp(local, vec3(0.0), maxIdx);

    ivec3 base = ivec3(floor(local));
    vec3  f    = local - vec3(base);

    for (int dz = 0; dz < 2; dz++) {
        for (int dy = 0; dy < 2; dy++) {
            for (int dx = 0; dx < 2; dx++) {
                ivec3 idx = min(base + ivec3(dx, dy, dz),
                                ivec3(giGridDimensions.xyz) - 1);
                vec3 w3 = mix(1.0 - f, f, vec3(dx, dy, dz));
                float w = w3.x * w3.y * w3.z;
                if (w <= 0.0) continue;
                uint flat_i = uint(idx.x)
                            + giGridDimensions.x
                              * (uint(idx.y) + giGridDimensions.y * uint(idx.z));
                for (int s = 0; s < 9; s++) {
                    sh[s] += gi_probe_sh[flat_i * 9u + uint(s)] * w;
                }
            }
        }
    }
}

// SH L2 → 法線方向の irradiance (cosine 畳み込み、 1/π 込み —
// pictor::sh_eval_irradiance と同一規約)。
vec3 giEvalIrradiance(vec4 sh[9], vec3 n) {
    const float A0 = 1.0;
    const float A1 = 2.0 / 3.0;
    const float A2 = 0.25;
    float b[9];
    b[0] = 0.282095;
    b[1] = 0.488603 * n.y;
    b[2] = 0.488603 * n.z;
    b[3] = 0.488603 * n.x;
    b[4] = 1.092548 * n.x * n.y;
    b[5] = 1.092548 * n.y * n.z;
    b[6] = 0.315392 * (3.0 * n.z * n.z - 1.0);
    b[7] = 1.092548 * n.x * n.z;
    b[8] = 0.546274 * (n.x * n.x - n.y * n.y);
    const float band[9] = float[](A0, A1, A1, A1, A2, A2, A2, A2, A2);

    vec3 result = vec3(0.0);
    for (int i = 0; i < 9; i++) {
        result += sh[i].rgb * (b[i] * band[i]);
    }
    return max(result, vec3(0.0));
}

// worldPos / normal の間接光 irradiance (giIntensity 済み)。
vec3 sampleGIIrradiance(vec3 worldPos, vec3 normal) {
    if (giIntensity <= 0.0) return vec3(0.0);
    vec4 sh[9];
    giInterpolateProbes(worldPos, sh);
    return giEvalIrradiance(sh, normal) * giIntensity;
}
