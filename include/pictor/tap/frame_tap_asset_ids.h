#pragma once

/// アセット名 / 指紋からの安定 ID (spec/feature/frame-tap.md §4)。
///
/// ID はビルドや登録順で変わらないこと。 ハンドル番号は使わない。

#include "pictor/tap/frame_tap_asset_source.h"
#include "pictor/tap/frame_tap_types.h"

#include <cstdint>
#include <string>

namespace pictor {

/// 名前の取れない draw に付けるタグ。
inline constexpr const char* kFrameTapTagUnnamed = "unnamed";

/// 頂点数 + インデックス数の FNV-1a 64bit (リトルエンディアン 8 バイトを順に混ぜる)。
uint64_t frame_tap_mesh_fingerprint(uint32_t vertex_count, uint32_t index_count);

/// 名前があれば名前、 無ければ "fp:" + 指紋の 16 桁小文字 hex を `out` に書く。
/// 名前があったら true (false なら呼び出し側が unnamed タグを付ける)。
bool resolve_mesh_id(const FrameTapMeshInfo& info, std::string& out);

/// UI 描画種別の ID ("ui:rect" / "ui:nine_slice" / "ui:image")。
const char* frame_tap_ui_mesh_id(FrameTapUiKind kind);

} // namespace pictor
