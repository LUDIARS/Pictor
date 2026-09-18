#version 450

layout(push_constant) uniform Push {
    mat4  dst_clip_from_src_ndc;
    ivec2 grid_cells;
    vec2  src_size;
    float min_texel_rate;
    float depth_bias;
} pc;

layout(set = 0, binding = 0) uniform sampler2D src_color;

layout(location = 0) in  vec2 v_src_uv;
layout(location = 0) out vec4 out_color;

void main() {
    // @implements SPEC-PC-XR-DELTA-RENDERING
    // 写し先の 1 画素あたり、 元の画を何画素ぶん進むか。 元の視点から見えていなかった
    // 面 (手前の物の陰) をまたぐ三角形は大きく引き伸ばされ、 この値が小さくなる。
    // そこは正しい色が無いので捨てて穴にし、 後段の穴埋め描画に任せる。
    vec2  step_x = dFdx(v_src_uv) * pc.src_size;
    vec2  step_y = dFdy(v_src_uv) * pc.src_size;
    float rate   = min(length(step_x), length(step_y));
    if (rate < pc.min_texel_rate) discard;

    out_color = textureLod(src_color, v_src_uv, 0.0);
}
