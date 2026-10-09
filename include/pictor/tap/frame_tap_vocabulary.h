#pragma once

/// render-tap/1 契約の語彙 (spec/feature/frame-tap.md §3)。
///
/// 契約の列挙値 (pass の種類 / 可視性の根拠 / 名前の付け方 / UI の役割 / 終了理由 /
/// 欠損理由) と、 それを JSON に出す文字列の対応だけを持つ。 値型は frame_tap_types.h。

#include <cstdint>
#include <string_view>

namespace pictor {

/// 契約の版。 各行の `contract` に入る。
inline constexpr const char* kFrameTapContract = "render-tap/1";

/// プレイヤーのカメラの観測者 ID (既定値)。
inline constexpr const char* kFrameTapPlayerObserver = "player-camera";

/// pass の種類。 scene / ui だけが観測者の画面に出うる (それ以外は診断用)。
enum class FrameTapPassKind : uint8_t {
    SCENE         = 0,
    UI            = 1,
    SHADOW        = 2,
    REFLECTION    = 3,
    DEPTH_PREPASS = 4,
    POSTPROCESS   = 5,
    OTHER         = 6,
};

/// draw が観測者の画面へ届いた根拠。 GPU を待たずに取れる範囲だけを言う。
enum class FrameTapVisibility : uint8_t {
    OCCLUSION_PASSED = 0,
    OCCLUSION_FAILED = 1,
    FRUSTUM_ONLY     = 2,  ///< 視錐台内だが遮蔽は不明 (Pictor の CPU カリングの既定)
    UNKNOWN          = 3,
};

/// mesh / material の名前の付け方。 値が大きいほど識別力が弱い。
enum class FrameTapIdentity : uint8_t {
    ASSET_NAME   = 0,  ///< アセット名由来の安定 ID
    CONTENT_HASH = 1,  ///< 頂点・インデックス内容のハッシュ
    COUNT_HASH   = 2,  ///< 頂点数 + インデックス数だけ (衝突する — 識別に使えない)
};

/// UI draw の意味 (ホストが与える)。 NONE なら `ui` を出さない。
enum class FrameTapUiRole : uint8_t {
    NONE  = 0,
    BAR   = 1,  ///< HP バー等。 fill (0..1) を持つ
    GLYPH = 2,  ///< 数字・文字。 glyph を持つ
};

/// end 行の理由。
enum class FrameTapEndReason : uint8_t {
    SHUTDOWN = 0,
    FAILED   = 1,  ///< "error" (wingdi.h の ERROR マクロを避けた名前)
};

/// フレーム内で記録しきれなかった draw の理由。
enum class FrameTapDropReason : uint8_t {
    BACKPRESSURE = 0,
    BUFFER_FULL  = 1,
    OTHER        = 2,  ///< 数値が有限でない / 契約の範囲外など
};

const char* frame_tap_pass_kind_name(FrameTapPassKind kind);
const char* frame_tap_visibility_name(FrameTapVisibility visibility);
const char* frame_tap_identity_name(FrameTapIdentity identity);
const char* frame_tap_ui_role_name(FrameTapUiRole role);
const char* frame_tap_end_reason_name(FrameTapEndReason reason);
const char* frame_tap_drop_reason_name(FrameTapDropReason reason);

/// 観測者 ID の書式 `^[a-z0-9][a-z0-9_.-]*$`。
bool is_valid_frame_tap_observer_id(std::string_view id);

} // namespace pictor
