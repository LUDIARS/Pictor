// Pictor — SHaRC 拡張: 共通定義 (セルレイアウト / パッキング / 空間ハッシュ)
//
// spec: E:/Document/Ars/pictor-sharc-ext-design.md (SHaRC拡張 ライティングキャッシュ設計)
// ワールド空間ハッシュセルに 2 層表現を載せる:
//   層1 モーメント M0/M1 (SSS・拡散用, 16B)
//   層2 位置つき vMF/SG ローブ ×SHARC_LOBE_COUNT (鏡面用, 12B×本数)
//   層3 メタデータ (分散 / サンプル数 / 信頼度, 16B)
// 既定 4 ローブで 1 セル = 80B (uvec4 × 5)。
// CPU 側レイアウトミラー: include/pictor/gi/sharc_types.h (バイト互換必須)。
//
// include 元シェーダは #version 450 + GL_GOOGLE_include_directive を宣言し、
// 事前に SHARC_SET (descriptor set 番号) を定義すること。

#ifndef PICTOR_SHARC_COMMON_GLSL
#define PICTOR_SHARC_COMMON_GLSL

#ifndef SHARC_SET
#define SHARC_SET 0
#endif

// ローブ本数はコンパイル時固定 (2/4/8)。既定 4 → セル 80B。
#ifndef SHARC_LOBE_COUNT
#define SHARC_LOBE_COUNT 4
#endif

const float SHARC_PI      = 3.14159265358979323846;
const float SHARC_INV_PI  = 0.31830988618379067154;
const float SHARC_EPSILON = 1e-6;

// ============================================================
// セルデータ (uvec4 × (2 + SHARC_LOBE_COUNT*3/4))
//   moments.x = M0 fluence φ (RGB9E5)
//   moments.y = M1 方向 (oct16, low) | 信頼度 unorm16 (high)
//   moments.z = M1 大きさ |E| (RGB9E5)
//   moments.w = 予約 (0)
//   lobes[i*3+0] = 方向 (oct16, low) | 鋭さ κ (half16, high)
//   lobes[i*3+1] = 強度 μ RGB (RGB9E5)
//   lobes[i*3+2] = 実効距離 (half16, low) | EM 重み π (half16, high)
//   meta.x = サンプル数 (low16) | 最終更新フレーム (high16)
//   meta.y = 輝度平均 (half16, low) | 輝度分散 (half16, high)
//   meta.z = ハッシュレベル (low8) | フラグ (high24)
//   meta.w = 予約 (0)
// ============================================================

struct SharcCellMoments {
    uvec4 packed;
};

struct SharcCellMeta {
    uvec4 packed;
};

// SSBO 上のセルは uint 列で持つ (std430 で 80B/セル を保証)。
const uint SHARC_CELL_UINTS_MOMENTS = 4u;
const uint SHARC_CELL_UINTS_LOBES   = uint(SHARC_LOBE_COUNT) * 3u;
const uint SHARC_CELL_UINTS_META    = 4u;
const uint SHARC_CELL_UINTS =
    SHARC_CELL_UINTS_MOMENTS + SHARC_CELL_UINTS_LOBES + SHARC_CELL_UINTS_META;

const uint SHARC_CELL_OFFSET_MOMENTS = 0u;
const uint SHARC_CELL_OFFSET_LOBES   = SHARC_CELL_UINTS_MOMENTS;
const uint SHARC_CELL_OFFSET_META    = SHARC_CELL_UINTS_MOMENTS
                                     + SHARC_CELL_UINTS_LOBES;

// ============================================================
// 共通パラメータ UBO
// ============================================================

layout(set = SHARC_SET, binding = 0) uniform SharcParams {
    vec4  sharcCameraPos;      // xyz = カメラ位置, w = 予約
    float sharcBaseCellSize;   // level 0 のセル一辺 (m)
    uint  sharcLevelCount;     // ハッシュ階層レベル数
    uint  sharcTableSize;      // ハッシュテーブルスロット数 (2^n)
    uint  sharcFrameIndex;     // EMA / エビクション用フレーム番号
    float sharcEmaAlpha;       // モーメント/ローブの時間平均係数 (0..1)
    float sharcLevelBias;      // 距離→レベル選択のバイアス
    float sharcSssMfpScale;    // MFP→レベル選択の倍率 (拡散半径 ≒ セルサイズ)
    uint  sharcMaxRaySteps;    // march の最大セルステップ数
    uint  sharcLightCount;     // ライトバッファ有効数
    uint  sharcRayCount;       // march 入力レイ数
    uint  sharcStaleFrames;    // これより古いセルはエビクション対象
    float sharcHitEpsilon;     // t 区間の数値マージン
};

// ============================================================
// ハッシュテーブル / セルストレージ SSBO
//   キー 0 = 空スロット。 キーは (grid, level) の 32bit 指紋で、
//   スロット位置は独立ハッシュ。 指紋衝突は確率的に許容 (SHaRC 同様)。
// ============================================================

layout(std430, set = SHARC_SET, binding = 1) buffer SharcHashKeys {
    uint gpu_sharc_keys[];         // [tableSize] 0=空
};

layout(std430, set = SHARC_SET, binding = 2) buffer SharcCells {
    uint gpu_sharc_cells[];        // [tableSize * SHARC_CELL_UINTS]
};

// スロット → グリッド座標の逆引き (セル本体 80B の外の補助データ)。
// 書くのは sharcInsertSlot の CAS 勝者のみ → 同フレーム内 race なし
// (読むのは後続パスのみ)。 x/y/z 各 signed 20bit + level 4bit を uvec2 に詰める。
layout(std430, set = SHARC_SET, binding = 11) buffer SharcCellPositions {
    uvec2 gpu_sharc_cell_pos[];    // [tableSize]
};

uvec2 sharcPackGridLevel(ivec3 grid, uint level) {
    uvec3 g = uvec3(grid + ivec3(1 << 19)) & uvec3(0xFFFFFu);
    uint lo = g.x | (g.y << 20);                    // y の下位 12bit まで
    uint hi = (g.y >> 12) | (g.z << 8) | ((level & 0xFu) << 28);
    return uvec2(lo, hi);
}

void sharcUnpackGridLevel(uvec2 p, out ivec3 grid, out uint level) {
    uint gx = p.x & 0xFFFFFu;
    uint gy = (p.x >> 20) | ((p.y & 0xFFu) << 12);
    uint gz = (p.y >> 8) & 0xFFFFFu;
    grid = ivec3(int(gx), int(gy), int(gz)) - ivec3(1 << 19);
    level = (p.y >> 28) & 0xFu;
}

// ============================================================
// パッキング
// ============================================================

// --- RGB9E5 (共有指数 5bit / 仮数 9bit×3) ---

uint sharcRgb9e5Encode(vec3 rgb) {
    const float kMax = 65408.0;    // (511/512) * 2^16
    rgb = clamp(rgb, vec3(0.0), vec3(kMax));
    float maxc = max(rgb.r, max(rgb.g, rgb.b));
    if (maxc < 1e-8) return 0u;
    int e = clamp(int(floor(log2(maxc))) + 1 + 15, 0, 31);
    float scale = exp2(float(e - 15 - 9));
    if (maxc / scale >= 511.5) {    // 丸めで仮数が溢れる場合は指数を繰り上げ
        e = min(e + 1, 31);
        scale = exp2(float(e - 15 - 9));
    }
    uvec3 m = min(uvec3(round(rgb / scale)), uvec3(511u));
    return (uint(e) << 27) | (m.b << 18) | (m.g << 9) | m.r;
}

vec3 sharcRgb9e5Decode(uint v) {
    float scale = exp2(float(int(v >> 27) - 15 - 9));
    return vec3(float(v & 0x1FFu),
                float((v >> 9) & 0x1FFu),
                float((v >> 18) & 0x1FFu)) * scale;
}

// --- oct16 方向 (8bit × 2, 単位ベクトル) ---

vec2 sharcSignNotZero(vec2 v) {
    return vec2(v.x >= 0.0 ? 1.0 : -1.0, v.y >= 0.0 ? 1.0 : -1.0);
}

uint sharcOct16Encode(vec3 n) {
    n /= max(abs(n.x) + abs(n.y) + abs(n.z), SHARC_EPSILON);
    vec2 p = (n.z >= 0.0) ? n.xy : (1.0 - abs(n.yx)) * sharcSignNotZero(n.xy);
    uvec2 q = uvec2(clamp(round(p * 127.0 + 127.5), 0.0, 255.0));
    return (q.y << 8) | q.x;
}

vec3 sharcOct16Decode(uint v) {
    vec2 p = (vec2(float(v & 0xFFu), float((v >> 8) & 0xFFu)) - 127.5) / 127.0;
    vec3 n = vec3(p, 1.0 - abs(p.x) - abs(p.y));
    if (n.z < 0.0) n.xy = (1.0 - abs(n.yx)) * sharcSignNotZero(n.xy);
    return normalize(n);
}

// --- half16 ペア (packHalf2x16 の別名, 意図明示用) ---

uint sharcHalf2Pack(float lo, float hi)  { return packHalf2x16(vec2(lo, hi)); }
vec2 sharcHalf2Unpack(uint v)            { return unpackHalf2x16(v); }

// --- unorm16 (信頼度など 0..1) ---

uint  sharcUnorm16Pack(float v) { return uint(round(clamp(v, 0.0, 1.0) * 65535.0)); }
float sharcUnorm16Unpack(uint v){ return float(v & 0xFFFFu) / 65535.0; }

// ============================================================
// 空間ハッシュ (グリッド座標 + レベル)
// ============================================================

float sharcCellSize(uint level) {
    return sharcBaseCellSize * exp2(float(level));
}

// カメラ距離ベースのレベル選択 (鏡面 / 汎用)。
uint sharcLevelFromDistance(float dist) {
    float l = log2(max(dist * sharcLevelBias / sharcBaseCellSize, 1.0));
    return min(uint(l), sharcLevelCount - 1u);
}

// MFP (平均自由行程) ベースのレベル選択 (SSS 用)。
// 拡散プロファイル積分半径 ≒ セルサイズになるレベルを引く (spec §2)。
uint sharcLevelFromMfp(float mfp) {
    float radius = max(mfp * sharcSssMfpScale, sharcBaseCellSize);
    float l = log2(radius / sharcBaseCellSize);
    return min(uint(round(l)), sharcLevelCount - 1u);
}

ivec3 sharcGridCoord(vec3 worldPos, uint level) {
    return ivec3(floor(worldPos / sharcCellSize(level)));
}

vec3 sharcCellCenter(ivec3 grid, uint level) {
    return (vec3(grid) + 0.5) * sharcCellSize(level);
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

// (grid, level) → 32bit 指紋。 0 は空スロット識別に予約するので回避。
uint sharcFingerprint(ivec3 grid, uint level) {
    uint h = sharcHashCombine(0x811C9DC5u, uint(grid.x));
    h = sharcHashCombine(h, uint(grid.y));
    h = sharcHashCombine(h, uint(grid.z));
    h = sharcHashCombine(h, level);
    return (h == 0u) ? 1u : h;
}

// (grid, level) → テーブル開始スロット (指紋とは独立のハッシュ)。
uint sharcSlotHash(ivec3 grid, uint level) {
    uint h = sharcHashCombine(0x9747B28Cu, uint(grid.x));
    h = sharcHashCombine(h, uint(grid.z));
    h = sharcHashCombine(h, uint(grid.y));
    h = sharcHashCombine(h, level);
    return h & (sharcTableSize - 1u);   // tableSize は 2^n 前提
}

const uint SHARC_PROBE_LIMIT = 16u;     // 線形走査の上限
const uint SHARC_SLOT_INVALID = 0xFFFFFFFFu;

// 検索のみ (挿入なし)。 見つからなければ SHARC_SLOT_INVALID。
uint sharcFindSlot(ivec3 grid, uint level) {
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

// 挿入 (atomicCAS)。 戻り値 = スロット。 満杯なら SHARC_SLOT_INVALID。
// outInserted = このスレッドが新規確保したか (要求リスト登録の重複排除)。
uint sharcInsertSlot(ivec3 grid, uint level, out bool outInserted) {
    uint fp = sharcFingerprint(grid, level);
    uint slot = sharcSlotHash(grid, level);
    outInserted = false;
    for (uint i = 0u; i < SHARC_PROBE_LIMIT; i++) {
        uint s = (slot + i) & (sharcTableSize - 1u);
        uint prev = atomicCompSwap(gpu_sharc_keys[s], 0u, fp);
        if (prev == 0u) { outInserted = true; return s; }
        if (prev == fp) return s;
    }
    return SHARC_SLOT_INVALID;
}

// ============================================================
// セルフィールドアクセス
// ============================================================

uint sharcCellBase(uint slot) { return slot * SHARC_CELL_UINTS; }

void sharcLoadMoments(uint slot, out vec3 m0, out vec3 m1Dir, out vec3 m1Mag,
                      out float confidence) {
    uint b = sharcCellBase(slot) + SHARC_CELL_OFFSET_MOMENTS;
    m0    = sharcRgb9e5Decode(gpu_sharc_cells[b + 0u]);
    uint dirConf = gpu_sharc_cells[b + 1u];
    m1Dir = sharcOct16Decode(dirConf & 0xFFFFu);
    confidence = sharcUnorm16Unpack(dirConf >> 16);
    m1Mag = sharcRgb9e5Decode(gpu_sharc_cells[b + 2u]);
}

void sharcStoreMoments(uint slot, vec3 m0, vec3 m1Dir, vec3 m1Mag,
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
    vec2 lv = sharcHalf2Unpack(gpu_sharc_cells[b + 1u]);
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

float sharcLuminance(vec3 rgb) {
    return dot(rgb, vec3(0.2126, 0.7152, 0.0722));
}

#endif // PICTOR_SHARC_COMMON_GLSL
