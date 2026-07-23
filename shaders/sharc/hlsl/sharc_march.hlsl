// Pictor — SHaRC 拡張 (DirectX 12 版) Pass 1: March / Trace
//
// GLSL 正本: shaders/sharc/sharc_march.comp (アルゴリズム変更禁止 — 忠実移植)。
// レイごとにハッシュグリッドを DDA で歩き、 触れたセルを spatial hash へ
// atomic 登録 (CAS 重複排除) しつつ、 ヒット列 (ray, slot, t区間) を SoA 保存。
//
// dispatch: (rayCount / 64, 1, 1)

#include "sharc_common.hlsli"

static const uint SHARC_HIT_CAPACITY_MARGIN = 1u;

// 触れたセルを登録し、 今フレーム初出なら要求リストに積む。
uint sharcRegisterCell(int3 grid, uint level) {
    bool inserted;
    uint slot = sharcInsertSlot(grid, level, inserted);
    if (slot == SHARC_SLOT_INVALID) return slot;

    if (inserted) {
        // 新規セルはメタ + 逆引き座標を初期化 (CAS 勝者のみが書く)
        sharcStoreMeta(slot, 0u, sharcFrameIndex & 0xFFFFu, 0.0, 0.0, level);
        gpu_sharc_cell_pos[slot] = sharcPackGridLevel(grid, level);
    }
    // 通常読みで既登録なら atomic を踏まない (多数派の fast path)
    if (gpu_sharc_stamps[slot] != sharcFrameIndex) {
        uint prevStamp;
        InterlockedExchange(gpu_sharc_stamps[slot], sharcFrameIndex, prevStamp);
        if (prevStamp != sharcFrameIndex) {
            uint idx;
            InterlockedAdd(gpu_sharc_counters[1], 1u, idx);
            if (idx < sharcTableSize) {
                gpu_sharc_requests[idx] = slot;
            }
        }
    }
    return slot;
}

[numthreads(64, 1, 1)]
void main(uint3 dtid : SV_DispatchThreadID) {
    uint rayIdx = dtid.x;
    if (rayIdx >= sharcRayCount) return;

    SharcRay ray = gpu_sharc_rays[rayIdx];
    float3 ro = ray.originTMin.xyz;
    float3 rd = ray.dirTMax.xyz;
    float t    = max(ray.originTMin.w, 0.0);
    float tMax = ray.dirTMax.w;

    uint hitsLen, hitsStride;
    gpu_sharc_hits.GetDimensions(hitsLen, hitsStride);

    for (uint step = 0u; step < sharcMaxRaySteps && t < tMax; step++) {
        float3 p = ro + rd * t;
        uint level = sharcLevelFromDistance(distance(p, sharcCameraPos.xyz));
        float cell = sharcCellSize(level);
        int3 grid = int3(floor(p / cell));

        // DDA: 現セルの exit t (数値誤差はマージンで押し出す)
        float3 cellMin = float3(grid) * cell;
        float3 cellMax = cellMin + cell;
        float3 tExit3 = float3(1e30, 1e30, 1e30);
        for (int a = 0; a < 3; a++) {
            if (rd[a] > SHARC_EPSILON)       tExit3[a] = (cellMax[a] - ro[a]) / rd[a];
            else if (rd[a] < -SHARC_EPSILON) tExit3[a] = (cellMin[a] - ro[a]) / rd[a];
        }
        float tExit = min(min(tExit3.x, tExit3.y), min(tExit3.z, tMax));

        uint slot = sharcRegisterCell(grid, level);
        if (slot != SHARC_SLOT_INVALID) {
            uint hitIdx;
            InterlockedAdd(gpu_sharc_counters[0], 1u, hitIdx);
            if (hitIdx + SHARC_HIT_CAPACITY_MARGIN < hitsLen) {
                SharcHit h;
                h.rayIdx = rayIdx;
                h.slot   = slot;
                h.t0     = t;
                h.t1     = tExit;
                gpu_sharc_hits[hitIdx] = h;
            }
        }

        t = tExit + sharcHitEpsilon;
    }

    // レイ終端 (= ヒット点) の周囲 2x2x2 セルを追加登録する (resolve のトライ
    // リニア補間評価がセル境界の切れ目対策で近傍セルのデータを必要とするため)。
    float3 endPos = ro + rd * tMax;
    uint endLevel = sharcLevelFromDistance(distance(endPos, sharcCameraPos.xyz));
    float endCell = sharcCellSize(endLevel);
    int3 endBase = int3(floor(endPos / endCell - 0.5));
    for (int c = 0; c < 8; c++) {
        int3 offs = int3(c & 1, (c >> 1) & 1, (c >> 2) & 1);
        sharcRegisterCell(endBase + offs, endLevel);
    }
}
