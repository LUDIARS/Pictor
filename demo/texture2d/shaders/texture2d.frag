#version 450

layout(push_constant) uniform PushConstants {
    mat4 projection;
    mat4 model;
    vec4 tint;     // rgb = tint, a = opacity
} pc;

layout(set = 0, binding = 0) uniform sampler2D u_texture;

layout(location = 0) in vec2 v_texCoord;
layout(location = 0) out vec4 fragColor;

void main() {
    vec4 texel = texture(u_texture, v_texCoord);
    fragColor  = vec4(texel.rgb * pc.tint.rgb, texel.a * pc.tint.a);
}
