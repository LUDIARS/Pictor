// Pictor — SHaRC ハイブリッド経路: G-buffer 頂点シェーダ (頂点プリング)
//
// 120fps 試算の (1): 一次可視性を compute BVH レイ (~80-90ms @720p/1070)
// からラスタライザ (~2-3ms) へ置き換える。 頂点バッファは持たず、
// SharcGpuExecutor のシーン SSBO (SharcTri, 葉順) を gl_VertexIndex で
// 直接引く (draw = tri_count * 3 頂点、 非インデックス)。
//
// レイアウトは shaders/sharc/sharc_scene.glsl の SharcTri と同一 (80B)。

#version 450

struct SharcTri {
    vec4 v0;           // xyz = 頂点, w = uintBitsToFloat(oct32 法線)
    vec4 v1;
    vec4 v2;
    vec4 uv01;         // xy = uv0, zw = uv1
    vec4 uv2Pad;       // xy = uv2
};

layout(std430, set = 0, binding = 0) readonly buffer GbTris {
    SharcTri gb_tris[];
};
layout(std430, set = 0, binding = 1) readonly buffer GbTriMats {
    uint gb_tri_mat[];
};
layout(std430, set = 0, binding = 2) readonly buffer GbTriAo {
    uint gb_tri_ao[];   // ロード時ベイクの頂点 AO (3 コーナー × unorm8)
};

layout(push_constant) uniform GbPush {
    mat4 gbViewProj;
    vec4 gbCameraPos;   // xyz = カメラ位置 (dist 計算用)
};

layout(location = 0) out vec3 outWorldPos;
layout(location = 1) out vec3 outNormal;
layout(location = 2) out vec2 outUv;
layout(location = 3) out float outAo;
layout(location = 4) flat out uint outMat;

vec3 gbOct32Decode(uint v) {
    vec2 p = (vec2(float(v & 0xFFFFu), float(v >> 16)) - 32767.5) / 32767.0;
    vec3 n = vec3(p, 1.0 - abs(p.x) - abs(p.y));
    if (n.z < 0.0) {
        n.xy = (1.0 - abs(n.yx)) *
               vec2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
    }
    return normalize(n);
}

void main() {
    uint triIdx = uint(gl_VertexIndex) / 3u;
    uint corner = uint(gl_VertexIndex) % 3u;
    SharcTri tri = gb_tris[triIdx];

    vec4 v = (corner == 0u) ? tri.v0 : (corner == 1u) ? tri.v1 : tri.v2;
    vec2 uv = (corner == 0u) ? tri.uv01.xy
            : (corner == 1u) ? tri.uv01.zw : tri.uv2Pad.xy;

    outWorldPos = v.xyz;
    outNormal   = gbOct32Decode(floatBitsToUint(v.w));
    outUv       = uv;
    outAo       = float((gb_tri_ao[triIdx] >> (8u * corner)) & 0xFFu) / 255.0;
    outMat      = gb_tri_mat[triIdx];
    gl_Position = gbViewProj * vec4(v.xyz, 1.0);
}
