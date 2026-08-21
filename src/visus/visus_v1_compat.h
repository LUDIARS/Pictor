#pragma once

// version 1 (typed スキーマ) の Visus JSON を v2 `VisusDesc` へ変換する
// (内部ヘッダ)。 `spec/feature/visus-v2-design.md` §2.2 の移行表に従う。
// KS `data/visus/` 既存 9 本の読込互換と `visus_migrate` (task 2) が使う。
// KS 移行完了後の次リリースで削除予定 (§5)。

#include "pictor/visus/visus.h"

#include <string>
#include <vector>

namespace pictor::visus_v1 {

/// v1 ルートオブジェクト → v2 VisusDesc。 構造不備は黙って default、
/// 戻り値は常に true (構文エラーは呼び出し側のパーサが弾く)。
/// `warnings` (non-null) に "v1 converted" と捨てた情報の注意を積む。
bool convert(const VisusMetadata&      root,
             VisusDesc&                out,
             std::vector<std::string>* warnings);

} // namespace pictor::visus_v1
