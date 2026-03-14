// Pictor — Ocean Wave Tessellation Control Shader
// Distance-based adaptive tessellation: more polygons near the camera,
// fewer polygons at distance. Targets ~100K total polygons across the
// ocean surface.

#version 450

layout(vertices = 4) out;

layout(set = 0, binding = 0) uniform SceneParams {
    mat4  view;
    mat4  projection;
    mat4  viewProjection;
    vec4  cameraPosition;
    float time;
    float maxTessLevel;     // maximum tessellation level (near camera)
    float minTessLevel;     // minimum tessellation level (far from camera)
    float tessNearDist;     // distance at which max tessellation is used
    float tessFarDist;      // distance beyond which min tessellation is used
    float pad0, pad1, pad2;
};

layout(location = 0) in vec3 vsPosition[];
layout(location = 1) in vec2 vsTexCoord[];

layout(location = 0) out vec3 tcsPosition[];
layout(location = 1) out vec2 tcsTexCoord[];

float calcTessLevel(vec3 p0, vec3 p1) {
    vec3 midpoint = (p0 + p1) * 0.5;
    float dist = distance(cameraPosition.xyz, midpoint);
    float t = clamp((dist - tessNearDist) / (tessFarDist - tessNearDist), 0.0, 1.0);
    return mix(maxTessLevel, minTessLevel, t);
}

void main() {
    tcsPosition[gl_InvocationID] = vsPosition[gl_InvocationID];
    tcsTexCoord[gl_InvocationID] = vsTexCoord[gl_InvocationID];

    if (gl_InvocationID == 0) {
        // Calculate tessellation levels based on edge midpoint distance to camera
        float e0 = calcTessLevel(vsPosition[0], vsPosition[3]); // left edge
        float e1 = calcTessLevel(vsPosition[0], vsPosition[1]); // bottom edge
        float e2 = calcTessLevel(vsPosition[1], vsPosition[2]); // right edge
        float e3 = calcTessLevel(vsPosition[2], vsPosition[3]); // top edge

        gl_TessLevelOuter[0] = e0;
        gl_TessLevelOuter[1] = e1;
        gl_TessLevelOuter[2] = e2;
        gl_TessLevelOuter[3] = e3;

        // Inner tessellation = average of edges
        gl_TessLevelInner[0] = (e1 + e3) * 0.5;
        gl_TessLevelInner[1] = (e0 + e2) * 0.5;
    }
}
