// Pictor — SHaRC 拡張: GPU シーン (フラット BVH) の共用交差ルーチン
//
// hit パス (一次交差 / シャドウレイ) と update パス (セル AO レイ) が
// 同じ BVH SSBO を辿るための共通定義。 include 前に SHARC_SET を定義し、
// sharc_common.glsl を先に include すること。
// BVH 規約は demo/sharc/mesh_bvh.cpp と同一: 左子 = 自ノード + 1。

#ifndef PICTOR_SHARC_SCENE_GLSL
#define PICTOR_SHARC_SCENE_GLSL

struct SharcBvhNode {
    vec4 bminLeft;     // xyz = AABB min, w = uintBitsToFloat(left)
    vec4 bmaxCount;    // xyz = AABB max, w = uintBitsToFloat(count)
};

layout(std430, set = SHARC_SET, binding = 14) readonly buffer SharcBvhNodes {
    SharcBvhNode gpu_sharc_bvh[];
};

struct SharcTri {
    vec4 v0;           // xyz = 頂点, w = uintBitsToFloat(oct32 法線 n0)
    vec4 v1;           // w = n1
    vec4 v2;           // w = n2
    vec4 uv01;         // xy = uv0, zw = uv1
    vec4 uv2Pad;       // xy = uv2, zw = 予約
};

layout(std430, set = SHARC_SET, binding = 15) readonly buffer SharcTris {
    SharcTri gpu_sharc_tris[];
};

layout(std430, set = SHARC_SET, binding = 16) readonly buffer SharcTriMats {
    uint gpu_sharc_tri_mat[];
};

struct SharcMaterial {
    vec4 albedoRough;  // rgb = albedo, a = roughness
    vec4 mfpPad;       // x = SSS MFP, y = atlas layer+1 (0 = テクスチャなし)
};

layout(std430, set = SHARC_SET, binding = 17) readonly buffer SharcMaterials {
    SharcMaterial gpu_sharc_materials[];
};

bool sharcAabbHit(vec3 bmin, vec3 bmax, vec3 ro, vec3 invRd, float tmax) {
    vec3 t0 = (bmin - ro) * invRd;
    vec3 t1 = (bmax - ro) * invRd;
    vec3 tlo = min(t0, t1);
    vec3 thi = max(t0, t1);
    float tEnter = max(max(tlo.x, tlo.y), max(tlo.z, 0.0));
    float tExit  = min(min(thi.x, thi.y), min(thi.z, tmax));
    return tEnter <= tExit;
}

// Möller–Trumbore。 ヒット時 t/u/v を返す。
bool sharcTriHit(SharcTri tri, vec3 ro, vec3 rd, float tmax,
                 out float outT, out float outU, out float outV) {
    vec3 e1 = tri.v1.xyz - tri.v0.xyz;
    vec3 e2 = tri.v2.xyz - tri.v0.xyz;
    vec3 pv = cross(rd, e2);
    float det = dot(e1, pv);
    if (abs(det) < 1e-12) return false;
    float invDet = 1.0 / det;
    vec3 tv = ro - tri.v0.xyz;
    float u = dot(tv, pv) * invDet;
    if (u < 0.0 || u > 1.0) return false;
    vec3 qv = cross(tv, e1);
    float v = dot(rd, qv) * invDet;
    if (v < 0.0 || u + v > 1.0) return false;
    float t = dot(e2, qv) * invDet;
    if (t <= 1e-4 || t >= tmax) return false;
    outT = t; outU = u; outV = v;
    return true;
}

// any-hit トラバーサル (シャドウ / AO レイ用 — 最初のヒットで打ち切り)。
bool sharcAnyHit(vec3 ro, vec3 rd, float tmax) {
    vec3 invRd = 1.0 / vec3(
        abs(rd.x) > 1e-12 ? rd.x : 1e-12,
        abs(rd.y) > 1e-12 ? rd.y : 1e-12,
        abs(rd.z) > 1e-12 ? rd.z : 1e-12);
    uint stack[48];
    int sp = 0;
    stack[sp++] = 0u;
    while (sp > 0) {
        uint ni = stack[--sp];
        SharcBvhNode node = gpu_sharc_bvh[ni];
        if (!sharcAabbHit(node.bminLeft.xyz, node.bmaxCount.xyz, ro, invRd,
                          tmax)) {
            continue;
        }
        uint count = floatBitsToUint(node.bmaxCount.w);
        if (count > 0u) {
            uint first = floatBitsToUint(node.bminLeft.w);
            for (uint i = 0u; i < count; i++) {
                float t, u, v;
                if (sharcTriHit(gpu_sharc_tris[first + i], ro, rd, tmax,
                                t, u, v)) {
                    return true;
                }
            }
        } else if (sp + 2 <= 48) {
            stack[sp++] = floatBitsToUint(node.bminLeft.w);
            stack[sp++] = ni + 1u;
        }
    }
    return false;
}

#endif // PICTOR_SHARC_SCENE_GLSL
