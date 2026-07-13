// Pictor — Bloom mip チェーン: downsample pass (fragment)
// 上位 mip (2 倍解像度) を 4-tap box (bilinear オフセット、 実質 16 texel
// 平均) で 1/2 に縮小する。 dual-filtering 系の軽量ダウンサンプル。
// texel_x/y はソース mip のテクセルサイズ。

#version 450

layout(location = 0) in  vec2 inUV;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D srcMip;

layout(push_constant) uniform BloomDownPC {
    float texel_x;
    float texel_y;
    float _pad0;
    float _pad1;
};

void main() {
    vec2 t = vec2(texel_x, texel_y);
    vec3 c = texture(srcMip, inUV + vec2(-t.x, -t.y)).rgb
           + texture(srcMip, inUV + vec2( t.x, -t.y)).rgb
           + texture(srcMip, inUV + vec2(-t.x,  t.y)).rgb
           + texture(srcMip, inUV + vec2( t.x,  t.y)).rgb;
    outColor = vec4(c * 0.25, 1.0);
}
