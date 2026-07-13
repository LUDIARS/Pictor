// Pictor — Per-Object Motion Blur (velocity buffer 方式) fullscreen pass
// scene pass MRT の velocity attachment (RG16F、 UV 差分) を直接読み、
// ピクセルごとの実速度に沿ってサンプルする — 動くオブジェクトにも
// ブラーが乗る (カメラ再投影方式 motion_blur.frag の上位互換、 phase 3)。
//
// binding 0 = シーンカラー (HDR)、 binding 1 = velocity (__velocity__)。
// valid = 0 で素通し。

#version 450

layout(location = 0) in  vec2 inUV;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D sceneColor;
layout(set = 0, binding = 1) uniform sampler2D velocityTex;

layout(push_constant) uniform MotionBlurVelocityPC {
    float intensity;
    float max_velocity;   // UV 単位クランプ
    uint  sample_count;   // 上限 16
    uint  valid;
};

void main() {
    vec3 center = texture(sceneColor, inUV).rgb;

    if (valid == 0u || intensity <= 0.0) {
        outColor = vec4(center, 1.0);
        return;
    }

    vec2 velocity = texture(velocityTex, inUV).rg * intensity;
    float speed = length(velocity);
    if (speed < 1e-5) {
        outColor = vec4(center, 1.0);
        return;
    }
    if (speed > max_velocity) {
        velocity *= max_velocity / speed;
    }

    uint samples = clamp(sample_count, 2u, 16u);
    vec3  sum = center;
    float weightSum = 1.0;
    for (uint i = 1u; i < samples; ++i) {
        float t = float(i) / float(samples - 1u) - 0.5;   // -0.5 .. +0.5
        vec2 uv = inUV + velocity * t;
        float inBounds = (uv == clamp(uv, vec2(0.0), vec2(1.0))) ? 1.0 : 0.0;
        float w = (1.0 - abs(t)) * inBounds;
        sum += texture(sceneColor, clamp(uv, vec2(0.0), vec2(1.0))).rgb * w;
        weightSum += w;
    }

    outColor = vec4(sum / weightSum, 1.0);
}
