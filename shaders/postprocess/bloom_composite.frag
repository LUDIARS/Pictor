// Pictor — Bloom Composite Fragment Shader
// Additively blends bloom result with the original HDR scene color.

#version 450

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D sceneColor;
layout(set = 0, binding = 1) uniform sampler2D bloomTexture;

layout(push_constant) uniform BloomCompositeParams {
    float intensity;
};

void main() {
    vec3 scene = texture(sceneColor, inUV).rgb;
    vec3 bloom = texture(bloomTexture, inUV).rgb;

    outColor = vec4(scene + bloom * intensity, 1.0);
}
