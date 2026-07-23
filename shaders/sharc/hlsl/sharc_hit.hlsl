// Pictor — SHaRC 拡張 (DirectX 12 版) Pass 0: Hit (GPU レイ生成 + 一次交差)
//
// GLSL 正本: shaders/sharc/sharc_hit.comp (アルゴリズム変更禁止 — 忠実移植)。
// フラット BVH (DoD 配列そのままの StructuredBuffer) を compute で辿る。
//
//   入力: カメラ基底 (cbuffer) — レイは画素 index から GPU 上で生成する。
//   出力: rays[] (march 用、 tMax = 実ヒット距離)
//         shade[] (位置 / 法線 / マテリアル / 視線)
//
// dispatch: (rayCount / 64, 1, 1)

#include "sharc_common.hlsli"

// ============================================================
// 交差
// ============================================================

bool sharcAabbHit(float3 bmin, float3 bmax, float3 ro, float3 invRd, float tmax) {
    float3 t0 = (bmin - ro) * invRd;
    float3 t1 = (bmax - ro) * invRd;
    float3 tlo = min(t0, t1);
    float3 thi = max(t0, t1);
    float tEnter = max(max(tlo.x, tlo.y), max(tlo.z, 0.0));
    float tExit  = min(min(thi.x, thi.y), min(thi.z, tmax));
    return tEnter <= tExit;
}

// Möller–Trumbore。 ヒット時 t/u/v を返す。
bool sharcTriHit(SharcTri tri, float3 ro, float3 rd, float tmax,
                 out float outT, out float outU, out float outV) {
    float3 e1 = tri.v1 - tri.v0;
    float3 e2 = tri.v2 - tri.v0;
    float3 pv = cross(rd, e2);
    float det = dot(e1, pv);
    outT = 0.0; outU = 0.0; outV = 0.0;
    if (abs(det) < 1e-12) return false;
    float invDet = 1.0 / det;
    float3 tv = ro - tri.v0;
    float u = dot(tv, pv) * invDet;
    if (u < 0.0 || u > 1.0) return false;
    float3 qv = cross(tv, e1);
    float v = dot(rd, qv) * invDet;
    if (v < 0.0 || u + v > 1.0) return false;
    float t = dot(e2, qv) * invDet;
    if (t <= 1e-4 || t >= tmax) return false;
    outT = t; outU = u; outV = v;
    return true;
}

[numthreads(64, 1, 1)]
void main(uint3 dtid : SV_DispatchThreadID) {
    uint rayIdx = dtid.x;
    if (rayIdx >= sharcRayCount) return;
    if ((sharcSceneFlags & SHARC_SCENE_MESH) == 0u) return;

    // ── GPU レイ生成: 画素 index → カメラレイ (CPU 転送なし) ──
    uint renderW = uint(sharcCamUp.w);
    uint px = rayIdx % renderW;
    uint py = rayIdx / renderW;
    uint renderH = sharcRayCount / renderW;
    float fovScale = sharcCamFwd.w;
    float aspect   = sharcCamRight.w;
    float u = (2.0 * (float(px) + 0.5) / float(renderW) - 1.0) * fovScale * aspect;
    float v = (1.0 - 2.0 * (float(py) + 0.5) / float(renderH)) * fovScale;
    float3 ro = sharcCameraPos.xyz;
    float3 rd = normalize(sharcCamFwd.xyz + sharcCamRight.xyz * u + sharcCamUp.xyz * v);
    float3 invRd = 1.0 / float3(
        abs(rd.x) > 1e-12 ? rd.x : 1e-12,
        abs(rd.y) > 1e-12 ? rd.y : 1e-12,
        abs(rd.z) > 1e-12 ? rd.z : 1e-12);

    float best = sharcSceneRayFar;
    uint bestTri = 0xFFFFFFFFu;
    float bestU = 0.0, bestV = 0.0;

    // ── BVH トラバーサル (私有スタック) ──
    uint stack[48];
    int sp = 0;
    stack[sp++] = 0u;
    while (sp > 0) {
        uint ni = stack[--sp];
        SharcBvhNode node = gpu_sharc_bvh[ni];
        if (!sharcAabbHit(node.bmin, node.bmax, ro, invRd, best)) {
            continue;
        }
        uint count = node.count;
        if (count > 0u) {
            uint first = node.left;
            for (uint i = 0u; i < count; i++) {
                float t, u2, v2;
                if (sharcTriHit(gpu_sharc_tris[first + i], ro, rd, best, t, u2, v2)) {
                    best = t;
                    bestTri = first + i;
                    bestU = u2; bestV = v2;
                }
            }
        } else if (sp + 2 <= 48) {
            stack[sp++] = node.left;   // right
            stack[sp++] = ni + 1u;     // left
        }
    }

    // ── 解析床 (D2 用チェッカー。 Bistro は実地面があるので flags off) ──
    bool floorHit = false;
    if ((sharcSceneFlags & SHARC_SCENE_FLOOR) != 0u && rd.y < -1e-5) {
        float t = (sharcFloorY - ro.y) / rd.y;
        if (t > 1e-4 && t < best) {
            best = t;
            bestTri = 0xFFFFFFFFu;
            floorHit = true;
        }
    }

    SharcShadeRequest req;
    if (bestTri != 0xFFFFFFFFu) {
        SharcTri tri = gpu_sharc_tris[bestTri];
        float3 pos = ro + rd * best;
        float w = 1.0 - bestU - bestV;
        float3 n = normalize(
            w * sharcOct32Decode(tri.n0) +
            bestU * sharcOct32Decode(tri.n1) +
            bestV * sharcOct32Decode(tri.n2));
        if (dot(n, rd) > 0.0) n = -n;   // 裏面ヒット (逆光透過) は視線側へ
        SharcMaterial mat = gpu_sharc_materials[gpu_sharc_tri_mat[bestTri]];
        float3 albedo = mat.albedo;
        float layerPlus1 = mat.atlas_layer_plus1;
        if (layerPlus1 > 0.5) {
            float2 uv = w * tri.uv0 + bestU * tri.uv1 + bestV * tri.uv2;
            float lod = clamp(log2(max(best, 1.0)), 0.0, 9.0);
            albedo *= sharcAlbedoAtlas.SampleLevel(
                sharcSampler, float3(uv, layerPlus1 - 1.0), lod).rgb;
        }
        req.posRough   = float4(pos, mat.roughness);
        req.normalMfp  = float4(n, mat.mfp);
        req.albedoView = float4(albedo, 0.0);
        req.viewDir    = float4(-rd, 0.0);
    } else if (floorHit) {
        float3 pos = ro + rd * best;
        int cx = int(floor(pos.x));
        int cz = int(floor(pos.z));
        float c = (((cx ^ cz) & 1) == 0) ? 0.55 : 0.30;
        req.posRough   = float4(pos, 0.35);
        req.normalMfp  = float4(0.0, 1.0, 0.0, 0.0);
        req.albedoView = float4(c, c, c, 0.0);
        req.viewDir    = float4(-rd, 0.0);
    } else {
        req.posRough   = float4(0.0, 0.0, 0.0, 1.0);
        req.normalMfp  = float4(0.0, 1.0, 0.0, 0.0);
        req.albedoView = float4(0.0, 0.0, 0.0, 0.0);
        req.viewDir    = float4(0.0, 1.0, 0.0, 0.0);
    }
    gpu_sharc_shade[rayIdx] = req;
    // march 用レイを書き出す (tMax = 実ヒット距離で区間を締める)
    SharcRay outRay;
    outRay.originTMin = float4(ro, 0.0);
    outRay.dirTMax    = float4(rd, best);
    gpu_sharc_rays[rayIdx] = outRay;
}
