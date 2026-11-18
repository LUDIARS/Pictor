#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// ============================================================
// 上限付きのファイル読み込み
//
// 信頼できないファイル (モデル / フォント / アニメーション / シェーダ / 設定) を
// 丸ごと読む全ローダが使う。 ファイルの大きさを確保の前に上限と比べ、 上限を
// 超えるもの・大きさが分からないもの (tellg 失敗) は 1 バイトも確保せずに断る。
// 上限値は pictor/core/parse_limits.h の kMax*FileBytes。
// ============================================================

namespace pictor {

enum class FileReadStatus : uint8_t {
    OK,
    OPEN_FAILED,    ///< 開けない
    SIZE_UNKNOWN,   ///< 大きさを取得できない (tellg が負)
    TOO_LARGE,      ///< max_bytes を超える
    READ_FAILED,    ///< 途中で読めなくなった (out は空)
};

/// 失敗理由の短い名前 ("ok" / "open_failed" / ...)。
const char* file_read_status_name(FileReadStatus status) noexcept;

/// path を丸ごと out へ読む。 大きさが max_bytes 以下のときだけ確保する。
/// 失敗したとき out は空。
FileReadStatus read_file_bounded(const std::string& path, std::size_t max_bytes,
                                 std::vector<uint8_t>& out);

/// テキスト版。 内容はバイト列のまま (文字コードの変換はしない)。
FileReadStatus read_text_file_bounded(const std::string& path, std::size_t max_bytes,
                                      std::string& out);

} // namespace pictor
