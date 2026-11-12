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

// ---- ファイル丸ごと読みの上限 (pictor/core/bounded_file.h) ----
// 上限を超えるファイルは確保の前に断る。 値の根拠は
// spec/plan/2026-11-11-lorica-adoption.md §6。

// モデル・モーション (FBX / BVH)。 ゲームの 1 アセットとして十分大きい値。
inline constexpr std::size_t kMaxModelFileBytes = std::size_t{512} << 20;
// フォント (TTF / OTF / TTC)。 CJK の Super OTC (~120 MiB) も入る値。
inline constexpr std::size_t kMaxFontFileBytes = std::size_t{256} << 20;
// ベクタアニメーション (Rive / Lottie)。
inline constexpr std::size_t kMaxVectorAnimationFileBytes = std::size_t{64} << 20;
// 設定・プロファイル JSON。 Visus JSON の上限 (visus_json::kMaxInputBytes) と同じ 16 MiB。
inline constexpr std::size_t kMaxConfigFileBytes = std::size_t{16} << 20;
// GI ベイク結果 (GIBakeSystem::save の出力)。
inline constexpr std::size_t kMaxBakeFileBytes = std::size_t{512} << 20;

// ---- FBX 1 文書の累計予算 (FBXParseLimits の既定値) ----
// 確保してよい累計バイト: ノード・プロパティの構造体 + 文字列 + 展開後の配列。
// 1 バイトのプロパティ型コードが ~170 バイトの FBXProperty になる増幅と、
// 256 MiB 以下の配列を多数並べた zip bomb を、 合計で止める。
inline constexpr std::size_t kMaxFbxDocumentBytes = std::size_t{2} << 30;
// ノード + プロパティの数。
inline constexpr std::size_t kMaxFbxElements = std::size_t{8} << 20;

} // namespace pictor::parse_limits
