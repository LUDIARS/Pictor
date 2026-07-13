// Pictor — SSGI: gather pass (half-res + 時間フィルタ)
// 画面内の明るい面からの 1 次バウンスをスクリーンスペースで集める。
// 各ピクセルで法線半球側の近傍タップを取り、 手前の lit ピクセルの色を
// 距離減衰 + 向き重みで加算する。 ノイズは history buffer
// (`__history:pp_ssgi__`) との時間混合で均す (phase 3)。
// 画面外の光は拾えない — probe GI の補助。
//
// binding 0 = シーンカラー (HDR)、 binding 1 = 深度、 binding 2 = history。
// 出力は half-res の間接光 (加算はしない — ssgi_apply が合成)。

#version 450

layout(location = 0) in  vec2 inUV;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D sceneColor;
layout(set = 0, binding = 1) uniform sampler2D sceneDepth;
layout(set = 0, binding = 2) uniform sampler2D historyGI;

layout(push_constant) uniform SsgiGatherPC {
    float proj_xx;
    float proj_yy;
    float near_plane;
    float far_plane;
    float radius_px;     // フル解像度 px
    float feedback;      // 時間フィルタの history 混合率
    float texel_x;       // フル解像度テクセル
    float texel_y;
    uint  sample_count;  // 上限 16
    float _pad0;
    float _pad1;
    float _pad2;
};

const float GOLDEN_ANGLE = 2.39996323;

float linearizeDepth(float d) {
    return near_plane * far_plane
         / max(far_plane - d * (far_plane - near_plane), 1e-6);
}

vec3 reconstructViewPos(vec2 uv, float d) {
    float z = linearizeDepth(d);
    vec2 ndc = uv * 2.0 - 1.0;
    return vec3(ndc.x * z / proj_xx, ndc.y * z / proj_yy, z);
}

void main() {
    float depth = texture(sceneDepth, inUV).r;
    if (depth >= 1.0) {                       // 空
        outColor = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    vec3 centerPos = reconstructViewPos(inUV, depth);

    // 深度勾配から面法線 (view 空間)。
    vec3 dx = dFdx(centerPos);
    vec3 dy = dFdy(centerPos);
    vec3 normal = normalize(cross(dy, dx));
    if (normal.z > 0.0) normal = -normal;

    // spiral タップで近傍の lit 面から放射を集める。
    vec2 texel = vec2(texel_x, texel_y);
    uint samples = min(sample_count, 16u);
    vec3 gi = vec3(0.0);
    float weightSum = 0.0;

    for (uint i = 0u; i < samples; ++i) {
        float t = (float(i) + 0.5) / float(samples);
        float angle = float(i) * GOLDEN_ANGLE;
        vec2 offset = vec2(cos(angle), sin(angle)) * sqrt(t) * radius_px;
        vec2 uv = inUV + offset * texel;
        if (uv != clamp(uv, vec2(0.0), vec2(1.0))) continue;

        float sd = texture(sceneDepth, uv).r;
        if (sd >= 1.0) continue;
        vec3 samplePos = reconstructViewPos(uv, sd);

        vec3 toSample = samplePos - centerPos;
        float dist2 = dot(toSample, toSample);
        if (dist2 < 1e-6) continue;
        vec3 dir = toSample * inversesqrt(dist2);

        // 受け手の cos 重み (法線半球のみ) × 距離減衰。
        float ndotl = dot(normal, dir);
        if (ndotl <= 0.0) continue;
        float falloff = 1.0 / (1.0 + dist2);

        gi += texture(sceneColor, uv).rgb * (ndotl * falloff);
        weightSum += ndotl * falloff;
    }
    if (weightSum > 0.0) gi /= weightSum;

    // 時間フィルタ — 同 UV の history と混合 (カメラ静止前提の簡易版。
    // 動きの速いカメラでは残像が出るが half-res + 低周波なので目立ちにくい)。
    vec3 history = texture(historyGI, inUV).rgb;
    vec3 result = mix(gi, history, clamp(feedback, 0.0, 0.98));

    outColor = vec4(result, 1.0);
}
