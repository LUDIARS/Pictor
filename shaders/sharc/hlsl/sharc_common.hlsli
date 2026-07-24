// Pictor — SHaRC 拡張 (DirectX 12 版): 共通定義 (セルレイアウト / パッキング / 空間ハッシュ)
//
// GLSL 正本: shaders/sharc/sharc_common.glsl (アルゴリズム変更禁止 — 忠実移植)。
// CPU 側レイアウトミラー: include/pictor/gi/sharc_types.h (バイト互換必須)。
//
// リソース割当 (root signature, spec 指示どおり):
//   b0       : SharcParams (root CBV, 128B)
//   u0..u12  : UAV テーブル (keys/cells/rays/hits/counters/requests/stamps/
//              indirect/reservoirs/cell_pos/shade/output/予備)
//   t0..t5   : SRV テーブル (lights/bvh_nodes/tris/tri_mats/materials/atlas)
//   s0       : static sampler (LINEAR, WRAP, mip LINEAR)
//
// cs_5_1 (D3DCompile, dxc 不要)。 全パス共通でこのヘッダ 1 本を include する。

#ifndef PICTOR_SHARC_COMMON_HLSLI
#define PICTOR_SHARC_COMMON_HLSLI

#ifndef SHARC_LOBE_COUNT
#define SHARC_LOBE_COUNT 4
#endif

static const float SHARC_PI      = 3.14159265358979323846;
static const float SHARC_INV_PI  = 0.31830988618379067154;
static const float SHARC_EPSILON = 1e-6;

// ============================================================
// セル定数 (uvec4 × (2 + SHARC_LOBE_COUNT*3/4)) — sharc_types.h と一致
// ============================================================

static const uint SHARC_CELL_UINTS_MOMENTS = 4u;
static const uint SHARC_CELL_UINTS_LOBES   = uint(SHARC_LOBE_COUNT) * 3u;
static const uint SHARC_CELL_UINTS_META    = 4u;
static const uint SHARC_CELL_UINTS =
    SHARC_CELL_UINTS_MOMENTS + SHARC_CELL_UINTS_LOBES + SHARC_CELL_UINTS_META;

static const uint SHARC_CELL_OFFSET_MOMENTS = 0u;
static const uint SHARC_CELL_OFFSET_LOBES   = SHARC_CELL_UINTS_MOMENTS;
static const uint SHARC_CELL_OFFSET_META    =
    SHARC_CELL_UINTS_MOMENTS + SHARC_CELL_UINTS_LOBES;

static const uint SHARC_PROBE_LIMIT  = 16u;
static const uint SHARC_SLOT_INVALID = 0xFFFFFFFFu;

static const uint SHARC_SCENE_MESH  = 1u;
static const uint SHARC_SCENE_FLOOR = 2u;

// ============================================================
// 共通パラメータ (root CBV, b0) — sharc_types.h::SharcParamsGpu (128B) と一致。
// 各グループはちょうど 4 スカラで 16B ベクトルへ収まるため、GLSL std140
// と同じオフセットに自動整列する (追加の packoffset 不要)。
// ============================================================

cbuffer SharcParams : register(b0) {
    float4 sharcCameraPos;     // xyz = カメラ位置, w = 予約
    float  sharcBaseCellSize;  // level 0 のセル一辺 (m)
    uint   sharcLevelCount;    // ハッシュ階層レベル数
    uint   sharcTableSize;     // ハッシュテーブルスロット数 (2^n)
    uint   sharcFrameIndex;    // EMA / エビクション用フレーム番号
    float  sharcEmaAlpha;      // 時間平均係数 (0..1)
    float  sharcLevelBias;     // 距離→レベル選択バイアス
    float  sharcSssMfpScale;   // MFP→レベル選択倍率
    uint   sharcMaxRaySteps;   // march の最大セルステップ数
    uint   sharcLightCount;    // ライトバッファ有効数
    uint   sharcRayCount;      // march 入力レイ数
    uint   sharcStaleFrames;   // エビクション閾値 (フレーム)
    float  sharcHitEpsilon;    // t 区間の数値マージン
    uint   sharcSceneTriCount; // GPU シーンの三角形数 (0 = hit パス無効)
    uint   sharcSceneFlags;    // bit0 = メッシュシーン有効, bit1 = 解析床
    float  sharcFloorY;        // 解析床の高さ
    float  sharcSceneRayFar;   // hit パスの初期 tMax
    float4 sharcCamFwd;        // xyz = 前方, w = fovScale (tan(fov/2))
    float4 sharcCamRight;      // xyz = 右, w = アスペクト比
    float4 sharcCamUp;         // xyz = 上, w = レンダリング幅 (float)
};

// ============================================================
// AoS 構造体 (StructuredBuffer は C 構造体同様の詰め — 16B 強制なし)。
// 各構造体は CPU 側ミラー (sharc_executor.h / sharc_types.h) とバイト一致。
// ============================================================

struct SharcRay {
    float4 originTMin;   // xyz = 原点, w = tMin
    float4 dirTMax;      // xyz = 方向 (正規化), w = tMax
};

struct SharcHit {
    uint  rayIdx;
    uint  slot;
    float t0;
    float t1;
};

struct SharcShadeRequest {
    float4 posRough;     // xyz = ヒット点, w = roughness
    float4 normalMfp;    // xyz = 法線 (外向き), w = SSS MFP (0 = SSS 無効)
    float4 albedoView;   // rgb = albedo, a = 予約
    float4 viewDir;      // xyz = ヒット点→カメラ方向, w = 予約
};

struct SharcLight {
    float4 posRadius;       // xyz = 位置, w = 半径
    float4 colorIntensity;  // rgb = 色, a = 強度
};

// BVH ノード (32B)。 左子 = 自ノード + 1、 right/leaf-start を left に格納。
struct SharcBvhNode {
    float3 bmin;
    uint   left;      // 内部: right 子 index / 葉: 三角形開始
    float3 bmax;
    uint   count;      // 0 = 内部 / >0 = 葉の三角形数
};

// 三角形 (80B)。 n = oct32 頂点法線、 uv = コーナー UV (REPEAT サンプル用の生値)。
struct SharcTri {
    float3 v0; uint n0;
    float3 v1; uint n1;
    float3 v2; uint n2;
    float2 uv0;
    float2 uv1;
    float2 uv2;
    uint   pad0;
    uint   pad1;
};

// マテリアル (32B)。 atlas_layer_plus1 は 0 = テクスチャなし。
struct SharcMaterial {
    float3 albedo;
    float  roughness;
    float  mfp;
    float  atlas_layer_plus1;
    float2 pad;
};

// ============================================================
// リソース束縛 (spec の UAV u0..u12 / SRV t0..t5 割当)
// ============================================================

RWStructuredBuffer<uint>              gpu_sharc_keys       : register(u0);
RWStructuredBuffer<uint>              gpu_sharc_cells      : register(u1);
RWStructuredBuffer<SharcRay>          gpu_sharc_rays       : register(u2);
RWStructuredBuffer<SharcHit>          gpu_sharc_hits       : register(u3);
RWStructuredBuffer<uint>              gpu_sharc_counters   : register(u4);
RWStructuredBuffer<uint>              gpu_sharc_requests   : register(u5);
RWStructuredBuffer<uint>              gpu_sharc_stamps     : register(u6);
RWStructuredBuffer<uint>              gpu_sharc_indirect   : register(u7);
RWStructuredBuffer<uint4>             gpu_sharc_reservoirs : register(u8);
RWStructuredBuffer<uint2>             gpu_sharc_cell_pos   : register(u9);
RWStructuredBuffer<SharcShadeRequest> gpu_sharc_shade      : register(u10);
RWStructuredBuffer<float4>            gpu_sharc_output     : register(u11);
// u12 は予備 (未使用)。

StructuredBuffer<SharcLight>    gpu_sharc_lights    : register(t0);
StructuredBuffer<SharcBvhNode>  gpu_sharc_bvh       : register(t1);
StructuredBuffer<SharcTri>      gpu_sharc_tris      : register(t2);
StructuredBuffer<uint>          gpu_sharc_tri_mat   : register(t3);
StructuredBuffer<SharcMaterial> gpu_sharc_materials : register(t4);
Texture2DArray                  sharcAlbedoAtlas    : register(t5);
SamplerState                    sharcSampler        : register(s0);

// ============================================================
// パッキング
// ============================================================

// --- RGB9E5 (共有指数 5bit / 仮数 9bit×3) ---

uint sharcRgb9e5Encode(float3 rgb) {
    const float kMax = 65408.0;    // (511/512) * 2^16
    rgb = clamp(rgb, float3(0.0, 0.0, 0.0), float3(kMax, kMax, kMax));
    float maxc = max(rgb.r, max(rgb.g, rgb.b));
    if (maxc < 1e-8) return 0u;
    int e = clamp(int(floor(log2(maxc))) + 1 + 15, 0, 31);
    float scale = exp2(float(e - 15 - 9));
    if (maxc / scale >= 511.5) {    // 丸めで仮数が溢れる場合は指数を繰り上げ
        e = min(e + 1, 31);
        scale = exp2(float(e - 15 - 9));
    }
    uint3 m = min(uint3(round(rgb / scale)), uint3(511u, 511u, 511u));
    return (uint(e) << 27) | (m.b << 18) | (m.g << 9) | m.r;
}

float3 sharcRgb9e5Decode(uint v) {
    float scale = exp2(float(int(v >> 27) - 15 - 9));
    return float3(float(v & 0x1FFu),
                  float((v >> 9) & 0x1FFu),
                  float((v >> 18) & 0x1FFu)) * scale;
}

// --- oct 方向 (単位ベクトル) ---

float2 sharcSignNotZero(float2 v) {
    return float2(v.x >= 0.0 ? 1.0 : -1.0, v.y >= 0.0 ? 1.0 : -1.0);
}

uint sharcOct32Encode(float3 n) {
    n /= max(abs(n.x) + abs(n.y) + abs(n.z), SHARC_EPSILON);
    float2 p = (n.z >= 0.0) ? n.xy : (1.0 - abs(n.yx)) * sharcSignNotZero(n.xy);
    uint2 q = uint2(clamp(round(p * 32767.0 + 32767.5), 0.0, 65535.0));
    return (q.y << 16) | q.x;
}

float3 sharcOct32Decode(uint v) {
    float2 p = (float2(float(v & 0xFFFFu), float(v >> 16)) - 32767.5) / 32767.0;
    float3 n = float3(p, 1.0 - abs(p.x) - abs(p.y));
    if (n.z < 0.0) n.xy = (1.0 - abs(n.yx)) * sharcSignNotZero(n.xy);
    return normalize(n);
}

uint sharcOct16Encode(float3 n) {
    n /= max(abs(n.x) + abs(n.y) + abs(n.z), SHARC_EPSILON);
    float2 p = (n.z >= 0.0) ? n.xy : (1.0 - abs(n.yx)) * sharcSignNotZero(n.xy);
    uint2 q = uint2(clamp(round(p * 127.0 + 127.5), 0.0, 255.0));
    return (q.y << 8) | q.x;
}

float3 sharcOct16Decode(uint v) {
    float2 p = (float2(float(v & 0xFFu), float((v >> 8) & 0xFFu)) - 127.5) / 127.0;
    float3 n = float3(p, 1.0 - abs(p.x) - abs(p.y));
    if (n.z < 0.0) n.xy = (1.0 - abs(n.yx)) * sharcSignNotZero(n.xy);
    return normalize(n);
}

// --- half16 ペア (packHalf2x16 の HLSL 相当。 x=lo→低16bit, y=hi→高16bit) ---

uint sharcHalf2Pack(float lo, float hi) {
    return (f32tof16(hi) << 16) | f32tof16(lo);
}
float2 sharcHalf2Unpack(uint v) {
    return float2(f16tof32(v & 0xFFFFu), f16tof32(v >> 16));
}

// --- unorm16 (信頼度など 0..1) ---

uint  sharcUnorm16Pack(float v)  { return uint(round(clamp(v, 0.0, 1.0) * 65535.0)); }
float sharcUnorm16Unpack(uint v) { return float(v & 0xFFFFu) / 65535.0; }

// ============================================================
// 空間ハッシュ (グリッド座標 + レベル)
// ============================================================

float sharcCellSize(uint level) {
    return sharcBaseCellSize * exp2(float(level));
}

uint sharcLevelFromDistance(float dist) {
    float l = log2(max(dist * sharcLevelBias / sharcBaseCellSize, 1.0));
    return min(uint(l), sharcLevelCount - 1u);
}

uint sharcLevelFromMfp(float mfp) {
    float radius = max(mfp * sharcSssMfpScale, sharcBaseCellSize);
    float l = log2(radius / sharcBaseCellSize);
    return min(uint(round(l)), sharcLevelCount - 1u);
}

int3 sharcGridCoord(float3 worldPos, uint level) {
    return int3(floor(worldPos / sharcCellSize(level)));
}

float3 sharcCellCenter(int3 grid, uint level) {
    return (float3(grid) + 0.5) * sharcCellSize(level);
}

// 32bit 整数ハッシュ (lowbias32, Chris Wellons)。
uint sharcHashUint(uint x) {
    x ^= x >> 16; x *= 0x7FEB352Du;
    x ^= x >> 15; x *= 0x846CA68Bu;
    x ^= x >> 16;
    return x;
}

uint sharcHashCombine(uint seed, uint v) {
    return seed ^ (sharcHashUint(v) + 0x9E3779B9u + (seed << 6) + (seed >> 2));
}

uint sharcFingerprint(int3 grid, uint level) {
    uint h = sharcHashCombine(0x811C9DC5u, uint(grid.x));
    h = sharcHashCombine(h, uint(grid.y));
    h = sharcHashCombine(h, uint(grid.z));
    h = sharcHashCombine(h, level);
    return (h == 0u) ? 1u : h;
}

uint sharcSlotHash(int3 grid, uint level) {
    uint h = sharcHashCombine(0x9747B28Cu, uint(grid.x));
    h = sharcHashCombine(h, uint(grid.z));
    h = sharcHashCombine(h, uint(grid.y));
    h = sharcHashCombine(h, level);
    return h & (sharcTableSize - 1u);
}

// 検索のみ (挿入なし)。 見つからなければ SHARC_SLOT_INVALID。
uint sharcFindSlot(int3 grid, uint level) {
    uint fp = sharcFingerprint(grid, level);
    uint slot = sharcSlotHash(grid, level);
    for (uint i = 0u; i < SHARC_PROBE_LIMIT; i++) {
        uint s = (slot + i) & (sharcTableSize - 1u);
        uint k = gpu_sharc_keys[s];
        if (k == fp) return s;
        if (k == 0u) return SHARC_SLOT_INVALID;
    }
    return SHARC_SLOT_INVALID;
}

// 挿入。 戻り値 = スロット。 満杯なら SHARC_SLOT_INVALID。
uint sharcInsertSlot(int3 grid, uint level, out bool outInserted) {
    uint fp = sharcFingerprint(grid, level);
    uint slot = sharcSlotHash(grid, level);
    outInserted = false;
    for (uint i = 0u; i < SHARC_PROBE_LIMIT; i++) {
        uint s = (slot + i) & (sharcTableSize - 1u);
        uint cur = gpu_sharc_keys[s];
        if (cur == fp) return s;
        if (cur == 0u) {
            uint prev;
            InterlockedCompareExchange(gpu_sharc_keys[s], 0u, fp, prev);
            if (prev == 0u) { outInserted = true; return s; }
            if (prev == fp) return s;
        }
    }
    return SHARC_SLOT_INVALID;
}

// ============================================================
// グリッド座標逆引きパック (uint2, sharc_types.h::sharc_pack_grid_level と同一)
// ============================================================

uint2 sharcPackGridLevel(int3 grid, uint level) {
    uint3 g = uint3(grid + int3(1 << 19, 1 << 19, 1 << 19)) & uint3(0xFFFFFu, 0xFFFFFu, 0xFFFFFu);
    uint lo = g.x | (g.y << 20);
    uint hi = (g.y >> 12) | (g.z << 8) | ((level & 0xFu) << 28);
    return uint2(lo, hi);
}

void sharcUnpackGridLevel(uint2 p, out int3 grid, out uint level) {
    uint gx = p.x & 0xFFFFFu;
    uint gy = (p.x >> 20) | ((p.y & 0xFFu) << 12);
    uint gz = (p.y >> 8) & 0xFFFFFu;
    grid = int3(int(gx), int(gy), int(gz)) - int3(1 << 19, 1 << 19, 1 << 19);
    level = (p.y >> 28) & 0xFu;
}

// ============================================================
// セルフィールドアクセス
// ============================================================

uint sharcCellBase(uint slot) { return slot * SHARC_CELL_UINTS; }

void sharcLoadMoments(uint slot, out float3 m0, out float3 m1Dir, out float3 m1Mag,
                      out float confidence) {
    uint b = sharcCellBase(slot) + SHARC_CELL_OFFSET_MOMENTS;
    m0 = sharcRgb9e5Decode(gpu_sharc_cells[b + 0u]);
    uint dirConf = gpu_sharc_cells[b + 1u];
    m1Dir = sharcOct16Decode(dirConf & 0xFFFFu);
    confidence = sharcUnorm16Unpack(dirConf >> 16);
    m1Mag = sharcRgb9e5Decode(gpu_sharc_cells[b + 2u]);
}

void sharcStoreMoments(uint slot, float3 m0, float3 m1Dir, float3 m1Mag,
                       float confidence) {
    uint b = sharcCellBase(slot) + SHARC_CELL_OFFSET_MOMENTS;
    gpu_sharc_cells[b + 0u] = sharcRgb9e5Encode(m0);
    gpu_sharc_cells[b + 1u] = sharcOct16Encode(m1Dir)
                            | (sharcUnorm16Pack(confidence) << 16);
    gpu_sharc_cells[b + 2u] = sharcRgb9e5Encode(m1Mag);
    gpu_sharc_cells[b + 3u] = 0u;
}

void sharcLoadMeta(uint slot, out uint sampleCount, out uint lastFrame,
                   out float lumaMean, out float lumaVar, out uint level) {
    uint b = sharcCellBase(slot) + SHARC_CELL_OFFSET_META;
    uint sc = gpu_sharc_cells[b + 0u];
    sampleCount = sc & 0xFFFFu;
    lastFrame   = sc >> 16;
    float2 lv = sharcHalf2Unpack(gpu_sharc_cells[b + 1u]);
    lumaMean = lv.x;
    lumaVar  = lv.y;
    level = gpu_sharc_cells[b + 2u] & 0xFFu;
}

void sharcStoreMeta(uint slot, uint sampleCount, uint lastFrame,
                    float lumaMean, float lumaVar, uint level) {
    uint b = sharcCellBase(slot) + SHARC_CELL_OFFSET_META;
    gpu_sharc_cells[b + 0u] = (min(sampleCount, 0xFFFFu))
                            | ((lastFrame & 0xFFFFu) << 16);
    gpu_sharc_cells[b + 1u] = sharcHalf2Pack(lumaMean, lumaVar);
    gpu_sharc_cells[b + 2u] = level & 0xFFu;
    gpu_sharc_cells[b + 3u] = 0u;
}

float sharcLuminance(float3 rgb) {
    return dot(rgb, float3(0.2126, 0.7152, 0.0722));
}

#endif // PICTOR_SHARC_COMMON_HLSLI
