// Pictor — SHaRC 拡張 (DirectX 12 版) Pass 2: Compact
//
// GLSL 正本: shaders/sharc/sharc_compact.comp (アルゴリズム変更禁止 — 忠実移植)。
// march が積んだ要求セルリストを ExecuteIndirect 用 D3D12_DISPATCH_ARGUMENTS に
// 変換する。 (1) dispatch 引数生成 (2) 古いセルのエビクション。
//
// dispatch: (tableSize / 256, 1, 1)

#include "sharc_common.hlsli"

[numthreads(256, 1, 1)]
void main(uint3 dtid : SV_DispatchThreadID) {
    uint slot = dtid.x;

    // --- (1) indirect 引数生成 (先頭スレッドのみ) — D3D12_DISPATCH_ARGUMENTS ---
    if (slot == 0u) {
        uint requestCount = min(gpu_sharc_counters[1], sharcTableSize);
        gpu_sharc_indirect[0] = requestCount;   // 1 threadgroup = 1 要求セル
        gpu_sharc_indirect[1] = 1u;
        gpu_sharc_indirect[2] = 1u;
    }

    // --- (2) エビクション ---
    if (slot >= sharcTableSize) return;
    if (gpu_sharc_keys[slot] == 0u) return;

    uint sampleCount, lastFrame, level;
    float lumaMean, lumaVar;
    sharcLoadMeta(slot, sampleCount, lastFrame, lumaMean, lumaVar, level);

    // フレーム番号は 16bit 巡回。 巡回差で経過を測る。
    uint now = sharcFrameIndex & 0xFFFFu;
    uint age = (now - lastFrame) & 0xFFFFu;
    if (age > sharcStaleFrames) {
        gpu_sharc_keys[slot] = 0u;   // キー解放 (セル本体は再確保時に初期化)
    }
}
