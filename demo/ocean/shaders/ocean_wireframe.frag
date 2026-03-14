// Pictor — Ocean Wireframe Fragment Shader
// Renders tessellation wireframe with a bright color for visibility

#version 450

layout(set = 0, binding = 0) uniform SceneParams {
    mat4  view;
    mat4  projection;
    mat4  viewProjection;
    vec4  cameraPosition;
    float time;
    float tessLevelHigh;
    float tessLevelMedium;
    float tessLevelLow;
    float tessDistNearMid;
    float tessDistMidFar;
    float pad0, pad1;
};

layout(location = 0) in vec3 fragWorldPos;
layout(location = 1) in vec3 fragNormal;
layout(location = 2) in vec2 fragTexCoord;
layout(location = 3) in float fragFoam;

layout(location = 0) out vec4 outColor;

void main() {
    // 3-level tessellation visualization:
    //   High (near) = bright green, Medium = yellow, Low (far) = red
    float dist = length(fragWorldPos - cameraPosition.xyz);

    vec3 highColor   = vec3(0.0, 1.0, 0.5);   // bright green (near / high detail)
    vec3 mediumColor = vec3(1.0, 1.0, 0.0);   // yellow (medium detail)
    vec3 lowColor    = vec3(1.0, 0.3, 0.1);   // red-orange (far / low detail)

    vec3 wireColor;
    if (dist < tessDistNearMid) {
        float t = clamp(dist / tessDistNearMid, 0.0, 1.0);
        wireColor = mix(highColor, mediumColor, t);
    } else if (dist < tessDistMidFar) {
        float t = clamp((dist - tessDistNearMid) / (tessDistMidFar - tessDistNearMid), 0.0, 1.0);
        wireColor = mix(mediumColor, lowColor, t);
    } else {
        wireColor = lowColor;
    }

    // Slight brightness variation based on surface normal for depth perception
    float facing = abs(dot(normalize(fragNormal), vec3(0.0, 1.0, 0.0)));
    wireColor *= 0.6 + 0.4 * facing;

    outColor = vec4(wireColor, 1.0);
}
