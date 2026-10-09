#pragma once

/// フレームタップの値型 — 1 フレーム分の描画リスト (spec/feature/frame-tap.md §3)。
///
/// 出力形式の正本は Commentarii の render-tap 契約。ここでは Pictor 側で組み立てる
/// 中間表現だけを持ち、 文字列化は frame_tap_json_line.h が担う。

#include "pictor/core/types.h"

#include <cstdint>
#include <string>
#include <vector>

namespace pictor {

/// 描画リストの区分。 ポストプロセス pass は記録対象外なので値を持たない。
enum class FrameTapPass : uint8_t {
    SCENE = 0,
    UI    = 1,
};

/// 契約上の pass 名 ("scene" / "ui")。
const char* frame_tap_pass_name(FrameTapPass pass);

/// UI 描画の種別。 UIRenderer の UIDrawKind のうち実際に描く 3 種だけ
/// (Vulkan ヘッダに依存しないよう tap 側で持つ)。
enum class FrameTapUiKind : uint8_t {
    RECT       = 0,
    NINE_SLICE = 1,
    IMAGE      = 2,
};

/// 1 フレーム 1 回のカメラ。 viewport は screen_bbox の基準 (px)。
struct FrameTapCamera {
    float4x4 view       = float4x4::identity();
    float4x4 projection = float4x4::identity();
    uint32_t viewport_width  = 0;
    uint32_t viewport_height = 0;
};

/// 画面上の外接矩形 (px、 左上原点、 y 下向き)。
struct FrameTapScreenRect {
    float x = 0.0f;
    float y = 0.0f;
    float w = 0.0f;
    float h = 0.0f;
};

/// scene pass へ渡す 1 オブジェクト (SoA から読んだ値の束)。
struct FrameTapSceneObject {
    MeshHandle     mesh     = INVALID_MESH;
    MaterialHandle material = INVALID_MATERIAL;
    ObjectId       object   = INVALID_OBJECT_ID;
    float4x4       world    = float4x4::identity();
    AABB           world_bounds{};
};

/// ui pass へ渡す 1 描画 (画面 px の矩形)。
struct FrameTapUiDraw {
    FrameTapUiKind kind       = FrameTapUiKind::RECT;
    float          x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f;
    uint32_t       texture_id = 0;   ///< 0 = テクスチャ無し (純色)
    uint32_t       instance   = 0;   ///< 描画リスト内の添字
};

/// 出力 1 draw 分。 文字列は slot 再利用で容量を保つ (frame_tap.h)。
struct FrameTapDraw {
    FrameTapPass             pass = FrameTapPass::SCENE;
    std::string              mesh;
    std::vector<std::string> materials;
    uint64_t                 instance = 0;
    float4x4                 world    = float4x4::identity();
    FrameTapScreenRect       screen_bbox;
    /// 前後順を決めるための値 (出力しない)。 scene は視点からの深度、
    /// ui は記録順 (大きいほど後に描いた = 前)。
    float                    order_key   = 0.0f;
    uint32_t                 depth_order = 0;
    std::vector<std::string> tags;
};

/// 1 フレーム分の描画リスト。 `draw_count` 個だけが有効 (draws は slot を使い回す)。
struct FrameTapFrame {
    uint64_t                  frame = 0;
    double                    t     = 0.0;
    FrameTapCamera            camera;
    std::vector<FrameTapDraw> draws;
    size_t                    draw_count = 0;
};

} // namespace pictor
