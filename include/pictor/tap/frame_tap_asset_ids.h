#pragma once

/// アセット名 / 指紋からの安定 ID (spec/feature/frame-tap.md §4)。
///
/// ID はビルドや登録順で変わらないこと。 ハンドル番号は使わない。

#include "pictor/tap/frame_tap_asset_source.h"
#include "pictor/tap/frame_tap_types.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace pictor {

/// 名前の取れない draw に付けるタグ。
inline constexpr const char* kFrameTapTagUnnamed = "unnamed";

/// 頂点数 + インデックス数の FNV-1a 64bit (リトルエンディアン 8 バイトを順に混ぜる)。
/// identity は count-hash (衝突するので受信側は識別に使わない)。
uint64_t frame_tap_mesh_fingerprint(uint32_t vertex_count, uint32_t index_count);

/// 頂点バイト列とインデックスバイト列の FNV-1a 64bit (各バイト列の長さも混ぜる)。
/// identity は content-hash。 データが無ければ (両方 null / 0) false。
bool frame_tap_mesh_content_hash(const void* vertex_data, size_t vertex_size,
                                 const void* index_data, size_t index_size,
                                 uint64_t& out);

/// `out` にメッシュ ID を書き、 その名前の付け方を返す:
/// 名前があれば名前 (asset-name)、 内容ハッシュがあれば "ch:" + 16 桁 hex (content-hash)、
/// どちらも無ければ "fp:" + 指紋の 16 桁 hex (count-hash)。
FrameTapIdentity resolve_mesh_id(const FrameTapMeshInfo& info, std::string& out);

/// 外見 (mesh + 並べ替えた material[]) のハッシュ。 受信側の外見の鍵と同じ同値関係
/// (material の並び順に依らない)。 `scratch` は呼び出し間で使い回す作業領域。
uint64_t frame_tap_appearance_hash(std::string_view mesh,
                                   const std::vector<std::string>& materials,
                                   std::vector<uint64_t>& scratch);

/// UI 描画種別の ID ("ui:rect" / "ui:nine_slice" / "ui:image")。
const char* frame_tap_ui_mesh_id(FrameTapUiKind kind);

} // namespace pictor
