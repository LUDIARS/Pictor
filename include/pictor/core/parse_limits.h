#pragma once

#include <cstddef>
#include <cstdint>

// ============================================================
// パーサ DoS ハードニング上限
//
// 信頼できない入力 (FBX / BVH / 手書き JSON / フォント等) を処理する
// 全パーサが共有する「絶対上限」。 ファイル由来のカウント / 深さ /
// 出力量を無条件に信用しないための一括の防御パラメータ。
//
// 参照: review/2026-06-11/REVIEW_VULNERABILITY.md (V-4〜V-9)
// ============================================================

namespace pictor::parse_limits {

// 再帰下降パーサのネスト深度上限。 これを超えたらスタックオーバーフロー前に
// 失敗させる (FBX ノード木 / 手書き JSON の skip_value 相互再帰など)。
inline constexpr int kMaxNestingDepth = 256;

// 自前 inflate / 圧縮配列の展開が生成してよい出力バイト数の絶対上限 (256 MiB)。
// zip bomb (小さな入力 → 巨大出力) によるメモリ枯渇を抑止する。
inline constexpr std::size_t kMaxInflateBytes = std::size_t{256} << 20;

// cmap など「コードポイント範囲 → グリフ」展開の総挿入数上限 (全 Unicode 範囲)。
// 巨大レンジ (最大 ~40 億) の全展開による CPU / メモリ枯渇を抑止する。
inline constexpr std::uint32_t kMaxCodepointExpansion = 0x110000u;

} // namespace pictor::parse_limits
