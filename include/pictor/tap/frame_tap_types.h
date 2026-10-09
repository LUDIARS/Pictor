#pragma once

/// フレームタップの値型 — 1 フレーム分の描画リスト (spec/feature/frame-tap.md §3)。
///
/// 出力形式の正本は Commentarii の render-tap/1 契約。ここでは Pictor 側で組み立てる
/// 中間表現だけを持ち、 契約の語彙は frame_tap_vocabulary.h、 文字列化は
/// frame_tap_json_line.h が担う。

#include "pictor/core/types.h"
#include "pictor/tap/frame_tap_vocabulary.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace pictor {

/// 各フレームの先頭で必ず開く 2 つの pass の添字。 他の pass は FrameTap::add_pass() で足す。
inline constexpr uint32_t kFrameTapScenePass = 0;  ///< name "scene", kind scene
inline constexpr uint32_t kFrameTapUiPass    = 1;  ///< name "ui",    kind ui

/// ui draw の `instance` は scene の ObjectId (uint32) と重ならないようこの値を足して出す
/// (受信側は instance を pass 横断で照合する)。
inline constexpr uint64_t kFrameTapUiInstanceBase = uint64_t{1} << 32;

/// UI 描画の種別。 UIRenderer の UIDrawKind のうち実際に描く 3 種だけ
/// (Vulkan ヘッダに依存しないよう tap 側で持つ)。
enum class FrameTapUiKind : uint8_t {
    RECT       = 0,
    NINE_SLICE = 1,
    IMAGE      = 2,
};

/// 1 フレーム 1 回のカメラ。 viewport は screen_bbox の基準 (px)。 行列は Pictor の
/// 行ベクトル規約 (m[row][col]、 平行移動は m[3][0..2])。
struct FrameTapCamera {
    float4x4 view       = float4x4::identity();
    float4x4 projection = float4x4::identity();
    uint32_t viewport_width  = 0;
    uint32_t viewport_height = 0;
};

/// 画面上の矩形 (px、 左上原点、 y 下向き)。
struct FrameTapScreenRect {
    float x = 0.0f;
    float y = 0.0f;
    float w = 0.0f;
    float h = 0.0f;
};

/// scene 系 pass へ渡す 1 オブジェクト (SoA から読んだ値の束)。
struct FrameTapSceneObject {
    MeshHandle         mesh       = INVALID_MESH;
    MaterialHandle     material   = INVALID_MATERIAL;
    ObjectId           object     = INVALID_OBJECT_ID;
    float4x4           world      = float4x4::identity();
    AABB               world_bounds{};
    uint32_t           pass       = kFrameTapScenePass;
    FrameTapVisibility visibility = FrameTapVisibility::FRUSTUM_ONLY;
    /// 最終不透明度 (material × instance)。 負なら不明 (`alpha` を出さない = 契約上 1)。
    float              alpha      = -1.0f;
};

/// ui pass へ渡す 1 描画 (画面 px の矩形)。 文字列は借用 (add_ui_draw の中で複写する)。
struct FrameTapUiDraw {
    FrameTapUiKind     kind       = FrameTapUiKind::RECT;
    float              x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f;
    uint32_t           texture_id = 0;   ///< 0 = テクスチャ無し (純色)
    /// UI 内の instance (出力は kFrameTapUiInstanceBase + この値)。 UIRenderer は
    /// 描画リストの添字を入れ、 ホストが要素 ID を与えればそれを使う。
    uint32_t           instance   = 0;
    uint32_t           pass       = kFrameTapUiPass;
    float              alpha      = 1.0f;  ///< 色の a × opacity (0..1 に丸める)
    bool               has_clip   = false;
    FrameTapScreenRect clip;               ///< 所属パネルの矩形 (has_clip のとき)
    FrameTapUiRole     role       = FrameTapUiRole::NONE;
    std::string_view   element;            ///< 任意 (例 "hud.hp")
    float              fill       = 0.0f;  ///< role == BAR のとき 0..1
    std::string_view   glyph;              ///< role == GLYPH のとき (空なら ui を出さない)
};

/// ホストが UIRenderer の描画リストへ添える意味 (描画コマンドと同じ添字)。
/// UIRenderer::set_frame_tap_annotations() で渡す。
struct FrameTapUiAnnotation {
    /// UINT32_MAX なら描画リストの添字を instance にする。
    uint32_t       instance = UINT32_MAX;
    FrameTapUiRole role     = FrameTapUiRole::NONE;
    std::string    element;
    float          fill     = 0.0f;
    std::string    glyph;
};

/// 1 フレームの pass (出力順 = 開いた順)。
struct FrameTapPassInfo {
    std::string      name;
    FrameTapPassKind kind = FrameTapPassKind::SCENE;
};

/// 出力 1 draw 分。 文字列は slot 再利用で容量を保つ (frame_tap.h)。
struct FrameTapDraw {
    uint32_t                 pass = kFrameTapScenePass;
    std::string              mesh;
    std::vector<std::string> materials;
    FrameTapIdentity         identity   = FrameTapIdentity::ASSET_NAME;
    uint64_t                 instance   = 0;
    uint32_t                 generation = 0;
    float4x4                 world      = float4x4::identity();
    FrameTapScreenRect       screen_bbox;
    /// 前後順を決めるための値 (出力しない)。 scene 系は視点からの深度、
    /// ui は記録順 (小さいほど先に描いた)。
    float                    order_key   = 0.0f;
    uint32_t                 depth_order = 0;
    FrameTapVisibility       visibility  = FrameTapVisibility::FRUSTUM_ONLY;
    bool                     has_alpha   = false;
    float                    alpha       = 1.0f;
    bool                     has_clip    = false;
    FrameTapScreenRect       clip;
    FrameTapUiRole           ui_role     = FrameTapUiRole::NONE;
    std::string              ui_element;
    float                    ui_fill     = 0.0f;
    std::string              ui_glyph;
    std::vector<std::string> tags;
};

/// 1 フレーム分の描画リスト。 `pass_count` / `draw_count` 個だけが有効
/// (passes / draws は slot を使い回す)。
struct FrameTapFrame {
    uint64_t                      seq   = 0;
    uint64_t                      frame = 0;
    bool                          has_tick = false;
    uint64_t                      tick  = 0;
    double                        t     = 0.0;
    std::string                   observer = kFrameTapPlayerObserver;
    FrameTapCamera                camera;
    bool                          has_visibility_lag = false;
    uint32_t                      visibility_lag_frames = 0;
    std::vector<FrameTapPassInfo> passes;
    size_t                        pass_count = 0;
    std::vector<FrameTapDraw>     draws;
    size_t                        draw_count = 0;
    uint32_t                      dropped_draws = 0;
    FrameTapDropReason            dropped_reason = FrameTapDropReason::OTHER;
};

} // namespace pictor
