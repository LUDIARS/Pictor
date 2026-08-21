#pragma once

// Visus 用の最小 JSON パーサ / エミッタ (内部ヘッダ、 公開 API ではない)。
// 外部依存なし。 material_serializer / pipeline_profile_serializer と同じ
// 手書きパーサ方針。 ネスト深度は parse_limits::kMaxNestingDepth で打ち切る
// (DoS ガード、 review/2026-06-11 V-8)。

#include "pictor/visus/visus_metadata.h"

#include <string>
#include <string_view>

namespace pictor::visus_json {

inline constexpr size_t kMaxInputBytes = size_t{16} << 20;

/// JSON テキスト → VisusValue。 構文エラー / 深すぎるネスト / 過大入力は
/// false で `error` (non-null) に短い理由。 末尾の余分なトークンもエラー。
bool parse(std::string_view text, VisusValue& out, std::string* error = nullptr);

/// VisusValue → pretty-printed JSON (2 スペースインデント、 末尾改行あり)。
std::string emit(const VisusValue& value);

} // namespace pictor::visus_json
