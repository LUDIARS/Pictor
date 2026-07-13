// Pictor — TAA (velocity buffer 方式) fullscreen pass
// カメラ再投影の代わりに velocity attachment を使って前フレーム UV を求める。
// 動くオブジェクトの history 追跡が正確になり ghosting が減る (phase 3)。
// クランプ / feedback は taa.frag と同一。
//
// binding 0 = 現フレームカラー、 binding 1 = 深度 (未使用だが taa.frag と
// 入力形状を揃える — 将来の depth-reject 用)、 binding 2 = history、
// binding 3 = velocity (__velocity__)。

#version 450

layout(location = 0) in  vec2 inUV;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D currentColor;
layout(set = 0, binding = 1) uniform sampler2D sceneDepth;
layout(set = 0, binding = 2) uniform sampler2D historyColor;
layout(set = 0, binding = 3) uniform sampler2D velocityTex;

layout(push_constant) uniform TaaVelocityPC {
    float feedback_min;
    float feedback_max;
    float jitter_x;        // 現フレームのジッタ (px)
    float jitter_y;
    float texel_x;
    float texel_y;
    uint  valid;
    uint  _pad0;
};

vec3 rgbToYCoCg(vec3 c) {
    return vec3( 0.25 * c.r + 0.5 * c.g + 0.25 * c.b,
                 0.5  * c.r             - 0.5  * c.b,
                -0.25 * c.r + 0.5 * c.g - 0.25 * c.b);
}
vec3 yCoCgToRgb(vec3 c) {
    return vec3(c.x + c.y - c.z,
                c.x       + c.z,
                c.x - c.y - c.z);
}

void main() {
    vec2 unjitterUV = inUV - vec2(jitter_x * texel_x, jitter_y * texel_y);
    vec3 current = texture(currentColor, unjitterUV).rgb;

    if (valid == 0u) {
        outColor = vec4(current, 1.0);
        return;
    }

    // velocity は「現在 - 前フレーム」 の UV 差分 — 前フレーム UV へ戻す。
    vec2 velocity = texture(velocityTex, inUV).rg;
    vec2 prevUV = inUV - velocity;

    if (prevUV != clamp(prevUV, vec2(0.0), vec2(1.0))) {
        outColor = vec4(current, 1.0);
        return;
    }

    vec3 history = texture(historyColor, prevUV).rgb;

    // 3x3 近傍の YCoCg AABB クランプ (ghosting 抑制)。
    vec3 mn = rgbToYCoCg(current);
    vec3 mx = mn;
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            if (dx == 0 && dy == 0) continue;
            vec2 uv = unjitterUV + vec2(float(dx) * texel_x,
                                        float(dy) * texel_y);
            vec3 c = rgbToYCoCg(texture(currentColor, uv).rgb);
            mn = min(mn, c);
            mx = max(mx, c);
        }
    }
    history = yCoCgToRgb(clamp(rgbToYCoCg(history), mn, mx));

    float speedPx = length(velocity / vec2(texel_x, texel_y));
    float feedback = mix(feedback_max, feedback_min,
                         clamp(speedPx / 8.0, 0.0, 1.0));

    outColor = vec4(mix(current, history, feedback), 1.0);
}
