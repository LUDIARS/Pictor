#version 450

layout(location = 0) in vec2 fragTexCoord;
layout(location = 1) in vec4 fragColor;

layout(set = 0, binding = 0) uniform sampler2D texSampler;

layout(push_constant) uniform PushConstants {
    int  renderMode;    // 0 = textured, 1 = colored (no texture)
    int  effectMode;    // 0 = none, 1 = outline, 2 = shadow, 3 = glow
    vec4 outlineColor;  // outline/shadow/glow color
    float outlineWidth; // effect strength in UV space
} pc;

layout(location = 0) out vec4 outColor;

void main() {
    if (pc.renderMode == 1) {
        // Solid color mode (for backgrounds, labels)
        outColor = fragColor;
        return;
    }

    vec4 texColor = texture(texSampler, fragTexCoord);
    float alpha = texColor.a;

    if (pc.effectMode == 0) {
        // No effect — simple alpha-blended texture
        outColor = vec4(fragColor.rgb * texColor.rgb, alpha * fragColor.a);
    }
    else if (pc.effectMode == 1) {
        // Outline: sample surrounding texels, build outline from alpha difference
        vec2 texSize = vec2(textureSize(texSampler, 0));
        vec2 step = pc.outlineWidth / texSize;

        float maxAlpha = 0.0;
        // 8-tap sampling around current pixel
        maxAlpha = max(maxAlpha, texture(texSampler, fragTexCoord + vec2(-step.x, 0)).a);
        maxAlpha = max(maxAlpha, texture(texSampler, fragTexCoord + vec2( step.x, 0)).a);
        maxAlpha = max(maxAlpha, texture(texSampler, fragTexCoord + vec2(0, -step.y)).a);
        maxAlpha = max(maxAlpha, texture(texSampler, fragTexCoord + vec2(0,  step.y)).a);
        maxAlpha = max(maxAlpha, texture(texSampler, fragTexCoord + vec2(-step.x, -step.y)).a);
        maxAlpha = max(maxAlpha, texture(texSampler, fragTexCoord + vec2( step.x, -step.y)).a);
        maxAlpha = max(maxAlpha, texture(texSampler, fragTexCoord + vec2(-step.x,  step.y)).a);
        maxAlpha = max(maxAlpha, texture(texSampler, fragTexCoord + vec2( step.x,  step.y)).a);

        // Composite: outline behind, text in front
        float outlineAlpha = maxAlpha * (1.0 - alpha);
        vec3 color = mix(pc.outlineColor.rgb, fragColor.rgb * texColor.rgb, alpha);
        float finalAlpha = alpha * fragColor.a + outlineAlpha * pc.outlineColor.a;
        outColor = vec4(color, clamp(finalAlpha, 0.0, 1.0));
    }
    else if (pc.effectMode == 2) {
        // Drop shadow: offset sample
        vec2 texSize = vec2(textureSize(texSampler, 0));
        vec2 shadowOffset = pc.outlineWidth * vec2(2.0, 2.0) / texSize;
        float shadowAlpha = texture(texSampler, fragTexCoord - shadowOffset).a;

        float shadow = shadowAlpha * (1.0 - alpha) * pc.outlineColor.a;
        vec3 color = mix(pc.outlineColor.rgb, fragColor.rgb * texColor.rgb, alpha);
        float finalAlpha = alpha * fragColor.a + shadow;
        outColor = vec4(color, clamp(finalAlpha, 0.0, 1.0));
    }
    else if (pc.effectMode == 3) {
        // Glow: blurred surrounding alpha
        vec2 texSize = vec2(textureSize(texSampler, 0));
        vec2 step = pc.outlineWidth / texSize;

        float glowAlpha = 0.0;
        // 12-tap Gaussian-like sampling
        glowAlpha += texture(texSampler, fragTexCoord + vec2(-step.x, 0)).a * 0.15;
        glowAlpha += texture(texSampler, fragTexCoord + vec2( step.x, 0)).a * 0.15;
        glowAlpha += texture(texSampler, fragTexCoord + vec2(0, -step.y)).a * 0.15;
        glowAlpha += texture(texSampler, fragTexCoord + vec2(0,  step.y)).a * 0.15;
        glowAlpha += texture(texSampler, fragTexCoord + vec2(-step.x*2.0, 0)).a * 0.05;
        glowAlpha += texture(texSampler, fragTexCoord + vec2( step.x*2.0, 0)).a * 0.05;
        glowAlpha += texture(texSampler, fragTexCoord + vec2(0, -step.y*2.0)).a * 0.05;
        glowAlpha += texture(texSampler, fragTexCoord + vec2(0,  step.y*2.0)).a * 0.05;
        glowAlpha += texture(texSampler, fragTexCoord + vec2(-step.x, -step.y)).a * 0.05;
        glowAlpha += texture(texSampler, fragTexCoord + vec2( step.x, -step.y)).a * 0.05;
        glowAlpha += texture(texSampler, fragTexCoord + vec2(-step.x,  step.y)).a * 0.05;
        glowAlpha += texture(texSampler, fragTexCoord + vec2( step.x,  step.y)).a * 0.05;

        float glow = glowAlpha * (1.0 - alpha) * pc.outlineColor.a;
        vec3 color = mix(pc.outlineColor.rgb * glow, fragColor.rgb * texColor.rgb, alpha);
        float finalAlpha = alpha * fragColor.a + glow;
        outColor = vec4(color, clamp(finalAlpha, 0.0, 1.0));
    }
}
