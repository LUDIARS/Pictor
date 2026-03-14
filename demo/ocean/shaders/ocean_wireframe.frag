// Pictor — Ocean Wireframe Fragment Shader
// Renders tessellation wireframe with a bright color for visibility

#version 450

layout(set = 0, binding = 0) uniform SceneParams {
    mat4  view;
    mat4  projection;
    mat4  viewProjection;
    vec4  cameraPosition;
    float time;
    float maxTessLevel;
    float minTessLevel;
    float tessNearDist;
    float tessFarDist;
    float pad0, pad1, pad2;
};

layout(location = 0) in vec3 fragWorldPos;
layout(location = 1) in vec3 fragNormal;
layout(location = 2) in vec2 fragTexCoord;
layout(location = 3) in float fragFoam;

layout(location = 0) out vec4 outColor;

void main() {
    // Distance-based color: near = bright cyan, far = darker blue
    float dist = length(fragWorldPos - cameraPosition.xyz);
    float t = clamp(dist / tessFarDist, 0.0, 1.0);

    vec3 nearColor = vec3(0.0, 1.0, 0.9);   // bright cyan
    vec3 farColor  = vec3(0.1, 0.3, 0.6);   // muted blue

    vec3 wireColor = mix(nearColor, farColor, t);

    // Slight brightness variation based on surface normal for depth perception
    float facing = abs(dot(normalize(fragNormal), vec3(0.0, 1.0, 0.0)));
    wireColor *= 0.6 + 0.4 * facing;

    outColor = vec4(wireColor, 1.0);
}
