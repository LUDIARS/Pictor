// Pictor — Bloom mip チェーン: upsample + 合成 pass (fragment)
// 下位 mip (1/2 解像度) を 4-tap tent で引き伸ばし、 同レベルの downsample
// 結果 (skip 接続) に scatter 重みで加算する。 チェーンを上りながら
// 広がりの異なるブラーを蓄積し、 広く柔らかい bloom を作る。
// texel_x/y は下位 mip のテクセルサイズ。
//
// binding 0 = 下位 mip (upsample 結果 or 最下位 downsample)
// binding 1 = 同レベルの downsample 結果 (skip)

#version 450

layout(location = 0) in  vec2 inUV;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D lowerMip;
layout(set = 0, binding = 1) uniform sampler2D skipMip;

layout(push_constant) uniform BloomUpPC {
    float texel_x;
    float texel_y;
    float scatter;    // 下位 mip の寄与率 (0 = 上位のみ)
    float _pad0;
};

void main() {
    vec2 t = vec2(texel_x, texel_y);
    // 4-tap tent (半テクセルオフセット + bilinear で 9-tap 相当)。
    vec3 up = texture(lowerMip, inUV + vec2(-t.x * 0.5, -t.y * 0.5)).rgb
            + texture(lowerMip, inUV + vec2( t.x * 0.5, -t.y * 0.5)).rgb
            + texture(lowerMip, inUV + vec2(-t.x * 0.5,  t.y * 0.5)).rgb
            + texture(lowerMip, inUV + vec2( t.x * 0.5,  t.y * 0.5)).rgb;
    up *= 0.25;

    vec3 skip = texture(skipMip, inUV).rgb;
    outColor = vec4(mix(skip, up + skip, scatter), 1.0);
}
