#include "pictor/tap/frame_tap.h"

#include "pictor/core/transform_math.h"
#include "pictor/tap/frame_tap_asset_ids.h"
#include "pictor/tap/frame_tap_json_line.h"
#include "pictor/tap/frame_tap_projection.h"

#include <algorithm>
#include <cstdio>

namespace pictor {

namespace {

constexpr const char* kTagOffscreen = "offscreen";

/// 単位矩形 [0,1]^2 を画面 px の矩形へ写す行列 (行ベクトル規約: 平行移動は m[3])。
float4x4 ui_rect_world(const FrameTapUiDraw& draw) {
    float4x4 m = float4x4::identity();
    m.m[0][0] = draw.w;
    m.m[1][1] = draw.h;
    m.m[3][0] = draw.x;
    m.m[3][1] = draw.y;
    return m;
}

} // namespace

void FrameTap::set_sink(std::unique_ptr<IFrameTapSink> sink) {
    sink_             = std::move(sink);
    frame_open_       = false;
    frame_.draw_count = 0;
}

void FrameTap::begin_frame(uint64_t frame, double t, const FrameTapCamera& camera) {
    if (!sink_) return;
    frame_.frame      = frame;
    frame_.t          = t;
    frame_.camera     = camera;
    frame_.draw_count = 0;
    view_proj_        = transform_math::multiply(camera.view, camera.projection);
    ui_sequence_      = 0;
    frame_open_       = true;
}

bool FrameTap::accept_draw_() {
    if (!sink_) return false;
    if (frame_open_) return true;
    ++dropped_draws_;
    if (!warned_dropped_) {
        std::fprintf(stderr,
                     "[FrameTap] begin_frame 前 / end_frame 後に届いた draw を捨てました "
                     "(dropped_draws() で件数を確認できます)\n");
        warned_dropped_ = true;
    }
    return false;
}

FrameTapDraw& FrameTap::next_draw_(FrameTapPass pass) {
    if (frame_.draw_count == frame_.draws.size()) frame_.draws.emplace_back();
    FrameTapDraw& draw = frame_.draws[frame_.draw_count++];
    draw.pass = pass;
    draw.mesh.clear();
    draw.materials.clear();
    draw.instance    = 0;
    draw.world       = float4x4::identity();
    draw.screen_bbox = FrameTapScreenRect{};
    draw.order_key   = 0.0f;
    draw.depth_order = 0;
    draw.tags.clear();
    return draw;
}

void FrameTap::add_scene_draw(const FrameTapSceneObject& object) {
    if (!accept_draw_()) return;
    FrameTapDraw& draw = next_draw_(FrameTapPass::SCENE);

    const FrameTapMeshInfo mesh = assets_ ? assets_->mesh_info(object.mesh) : FrameTapMeshInfo{};
    bool named = resolve_mesh_id(mesh, draw.mesh);
    if (object.material != INVALID_MATERIAL) {
        const std::string_view material =
            assets_ ? assets_->material_name(object.material) : std::string_view();
        if (material.empty()) {
            named = false;  // ハンドル番号は出さない (§4)
        } else {
            draw.materials.emplace_back(material);
        }
    }

    draw.instance  = object.object;
    draw.world     = object.world;
    draw.order_key = frame_tap_view_depth(object.world_bounds, frame_.camera.view);

    const bool on_screen = project_world_aabb_to_screen(
        object.world_bounds, view_proj_,
        frame_.camera.viewport_width, frame_.camera.viewport_height, draw.screen_bbox);

    if (!named) draw.tags.emplace_back(kFrameTapTagUnnamed);
    if (!on_screen) draw.tags.emplace_back(kTagOffscreen);
}

void FrameTap::add_ui_draw(const FrameTapUiDraw& ui) {
    if (!accept_draw_()) return;
    FrameTapDraw& draw = next_draw_(FrameTapPass::UI);

    draw.mesh = frame_tap_ui_mesh_id(ui.kind);
    bool named = true;
    if (ui.texture_id != 0) {
        const std::string_view texture =
            assets_ ? assets_->ui_texture_name(ui.texture_id) : std::string_view();
        if (texture.empty()) {
            named = false;
        } else {
            draw.materials.emplace_back(texture);
        }
    }

    draw.instance    = ui.instance;
    draw.world       = ui_rect_world(ui);
    draw.screen_bbox = {ui.x, ui.y, ui.w, ui.h};
    // 後に描いたものほど前面 → order_key を小さくして depth_order 0 に寄せる。
    draw.order_key   = -static_cast<float>(ui_sequence_++);

    if (!named) draw.tags.emplace_back(kFrameTapTagUnnamed);
}

void FrameTap::assign_depth_order_(FrameTapPass pass) {
    order_scratch_.clear();
    for (size_t i = 0; i < frame_.draw_count; ++i) {
        if (frame_.draws[i].pass == pass) order_scratch_.push_back(static_cast<uint32_t>(i));
    }
    std::stable_sort(order_scratch_.begin(), order_scratch_.end(),
                     [this](uint32_t a, uint32_t b) {
                         return frame_.draws[a].order_key < frame_.draws[b].order_key;
                     });
    for (size_t rank = 0; rank < order_scratch_.size(); ++rank) {
        frame_.draws[order_scratch_[rank]].depth_order = static_cast<uint32_t>(rank);
    }
}

void FrameTap::end_frame() {
    if (!sink_ || !frame_open_) return;
    assign_depth_order_(FrameTapPass::SCENE);
    assign_depth_order_(FrameTapPass::UI);
    encode_frame_tap_line(frame_, line_);
    frame_open_ = false;
    sink_->write_line(line_);
    ++frames_written_;
}

} // namespace pictor
