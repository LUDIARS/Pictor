#include "pictor/tap/frame_tap.h"

#include "pictor/core/transform_math.h"
#include "pictor/tap/frame_tap_asset_ids.h"
#include "pictor/tap/frame_tap_json_line.h"
#include "pictor/tap/frame_tap_projection.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace pictor {

namespace {

constexpr const char* kTagOffscreen = "offscreen";
constexpr const char* kScenePassName = "scene";
constexpr const char* kUiPassName    = "ui";

/// 単位矩形 [0,1]^2 を画面 px の矩形へ写す行列 (行ベクトル規約: 平行移動は m[3])。
float4x4 ui_rect_world(const FrameTapUiDraw& draw) {
    float4x4 m = float4x4::identity();
    m.m[0][0] = draw.w;
    m.m[1][1] = draw.h;
    m.m[3][0] = draw.x;
    m.m[3][1] = draw.y;
    return m;
}

bool is_finite_matrix(const float4x4& m) {
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            if (!std::isfinite(m.m[r][c])) return false;
        }
    }
    return true;
}

bool is_finite_bounds(const AABB& b) {
    return std::isfinite(b.min.x) && std::isfinite(b.min.y) && std::isfinite(b.min.z) &&
           std::isfinite(b.max.x) && std::isfinite(b.max.y) && std::isfinite(b.max.z);
}

/// 契約の矩形: 有限で、 幅と高さが 0 以上。
bool is_contract_rect(float x, float y, float w, float h) {
    return std::isfinite(x) && std::isfinite(y) && std::isfinite(w) && std::isfinite(h) &&
           w >= 0.0f && h >= 0.0f;
}

void set_pass(FrameTapPassInfo& pass, std::string_view name, FrameTapPassKind kind) {
    pass.name.assign(name.data(), name.size());
    pass.kind = kind;
}

} // namespace

FrameTap::~FrameTap() {
    if (sink_) end_stream(FrameTapEndReason::SHUTDOWN);
}

void FrameTap::set_sink(std::unique_ptr<IFrameTapSink> sink) {
    if (sink_) {
        discard_open_frame_();
        write_end_line_(FrameTapEndReason::SHUTDOWN);
    }
    sink_       = std::move(sink);
    frame_open_ = false;
    next_seq_   = 0;
    clock_.reset_stream();
    generations_.reset();
}

void FrameTap::end_stream(FrameTapEndReason reason) {
    if (!sink_) return;
    discard_open_frame_();
    write_end_line_(reason);
    sink_.reset();
    next_seq_ = 0;
    clock_.reset_stream();
    generations_.reset();
}

bool FrameTap::set_observer_id(std::string_view id) {
    if (!is_valid_frame_tap_observer_id(id)) return false;
    observer_id_.assign(id.data(), id.size());
    return true;
}

void FrameTap::discard_open_frame_() {
    if (!frame_open_) return;
    frame_open_ = false;
    ++dropped_frames_;
}

void FrameTap::write_end_line_(FrameTapEndReason reason) {
    encode_frame_tap_end_line(next_seq_++, reason, line_);
    sink_->write_line(line_);
}

void FrameTap::begin_frame(uint64_t frame, double fallback_t, const FrameTapCamera& camera) {
    if (!sink_) return;
    discard_open_frame_();

    frame_.seq      = next_seq_++;
    frame_.frame    = frame;
    frame_.t        = clock_.next_time(fallback_t);
    frame_.has_tick = clock_.next_tick(frame_.tick);
    frame_.observer = observer_id_;
    frame_.camera   = camera;
    frame_.has_visibility_lag    = visibility_ != nullptr;
    frame_.visibility_lag_frames = visibility_ ? visibility_->lag_frames() : 0;
    frame_.draw_count     = 0;
    frame_.dropped_draws  = 0;
    frame_.dropped_reason = FrameTapDropReason::OTHER;

    if (frame_.passes.size() < 2) frame_.passes.resize(2);
    set_pass(frame_.passes[kFrameTapScenePass], kScenePassName, FrameTapPassKind::SCENE);
    set_pass(frame_.passes[kFrameTapUiPass], kUiPassName, FrameTapPassKind::UI);
    frame_.pass_count = 2;

    view_proj_   = transform_math::multiply(camera.view, camera.projection);
    ui_sequence_ = 0;
    frame_open_  = true;
}

uint32_t FrameTap::add_pass(std::string_view name, FrameTapPassKind kind) {
    if (!sink_ || !frame_open_ || name.empty()) return kInvalidPass;
    if (frame_.pass_count == frame_.passes.size()) frame_.passes.emplace_back();
    set_pass(frame_.passes[frame_.pass_count], name, kind);
    return static_cast<uint32_t>(frame_.pass_count++);
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

void FrameTap::count_frame_drop_(FrameTapDropReason reason) {
    ++dropped_draws_;
    // 1 フレームに理由が混ざったら buffer-full を優先する (記録量の問題を先に知らせる)。
    if (frame_.dropped_draws == 0 || reason == FrameTapDropReason::BUFFER_FULL) {
        frame_.dropped_reason = reason;
    }
    ++frame_.dropped_draws;
}

FrameTapDraw* FrameTap::next_draw_(uint32_t pass) {
    if (pass >= frame_.pass_count) {
        count_frame_drop_(FrameTapDropReason::OTHER);
        return nullptr;
    }
    if (max_draws_ != 0 && frame_.draw_count >= max_draws_) {
        count_frame_drop_(FrameTapDropReason::BUFFER_FULL);
        return nullptr;
    }
    if (frame_.draw_count == frame_.draws.size()) frame_.draws.emplace_back();
    FrameTapDraw& draw = frame_.draws[frame_.draw_count++];
    draw.pass = pass;
    draw.mesh.clear();
    draw.materials.clear();
    draw.identity    = FrameTapIdentity::ASSET_NAME;
    draw.instance    = 0;
    draw.generation  = 0;
    draw.world       = float4x4::identity();
    draw.screen_bbox = FrameTapScreenRect{};
    draw.order_key   = 0.0f;
    draw.depth_order = 0;
    draw.visibility  = FrameTapVisibility::UNKNOWN;
    draw.has_alpha   = false;
    draw.alpha       = 1.0f;
    draw.has_clip    = false;
    draw.clip        = FrameTapScreenRect{};
    draw.ui_role     = FrameTapUiRole::NONE;
    draw.ui_element.clear();
    draw.ui_fill     = 0.0f;
    draw.ui_glyph.clear();
    draw.tags.clear();
    return &draw;
}

void FrameTap::reject_last_draw_(FrameTapDropReason reason) {
    --frame_.draw_count;
    count_frame_drop_(reason);
}

bool FrameTap::assign_generation_(FrameTapDraw& draw, FrameTapInstanceSpace space,
                                  uint32_t key) {
    const uint64_t appearance =
        frame_tap_appearance_hash(draw.mesh, draw.materials, appearance_scratch_);
    return generations_.observe(space, key, appearance, draw.generation);
}

void FrameTap::add_scene_draw(const FrameTapSceneObject& object) {
    if (!accept_draw_()) return;
    FrameTapDraw* slot = next_draw_(object.pass);
    if (!slot) return;
    FrameTapDraw& draw = *slot;

    // 契約は数値必須で null を許さない。 有限でない値を含む draw は出さずに数える。
    const bool has_alpha = object.alpha >= 0.0f;
    if (!is_finite_matrix(object.world) || !is_finite_bounds(object.world_bounds) ||
        (has_alpha && !std::isfinite(object.alpha)) || std::isnan(object.alpha)) {
        reject_last_draw_(FrameTapDropReason::OTHER);
        return;
    }

    const FrameTapMeshInfo mesh = assets_ ? assets_->mesh_info(object.mesh) : FrameTapMeshInfo{};
    draw.identity = resolve_mesh_id(mesh, draw.mesh);
    bool named = draw.identity == FrameTapIdentity::ASSET_NAME;
    if (object.material != INVALID_MATERIAL) {
        const std::string_view material =
            assets_ ? assets_->material_name(object.material) : std::string_view();
        if (material.empty()) {
            // ハンドル番号は出さない。 外見の鍵が欠けるので識別に使えない側へ倒す (§4)。
            named         = false;
            draw.identity = FrameTapIdentity::COUNT_HASH;
        } else {
            draw.materials.emplace_back(material);
        }
    }

    draw.instance   = object.object;
    draw.world      = object.world;
    draw.order_key  = frame_tap_view_depth(object.world_bounds, frame_.camera.view);
    draw.visibility = object.visibility;
    if (has_alpha) {
        draw.has_alpha = true;
        draw.alpha     = std::min(object.alpha, 1.0f);
    }

    const bool on_screen = project_world_aabb_to_screen(
        object.world_bounds, view_proj_,
        frame_.camera.viewport_width, frame_.camera.viewport_height, draw.screen_bbox);

    if (!assign_generation_(draw, FrameTapInstanceSpace::SCENE, object.object)) {
        reject_last_draw_(FrameTapDropReason::OTHER);
        return;
    }

    if (!named) draw.tags.emplace_back(kFrameTapTagUnnamed);
    if (!on_screen) draw.tags.emplace_back(kTagOffscreen);
}

void FrameTap::add_ui_draw(const FrameTapUiDraw& ui) {
    if (!accept_draw_()) return;
    FrameTapDraw* slot = next_draw_(ui.pass);
    if (!slot) return;
    FrameTapDraw& draw = *slot;

    const bool bad_fill = ui.role == FrameTapUiRole::BAR &&
                          !(std::isfinite(ui.fill) && ui.fill >= 0.0f && ui.fill <= 1.0f);
    if (!is_contract_rect(ui.x, ui.y, ui.w, ui.h) || !std::isfinite(ui.alpha) ||
        (ui.has_clip && !is_contract_rect(ui.clip.x, ui.clip.y, ui.clip.w, ui.clip.h)) ||
        bad_fill) {
        reject_last_draw_(FrameTapDropReason::OTHER);
        return;
    }

    draw.mesh = frame_tap_ui_mesh_id(ui.kind);
    bool named = true;
    if (ui.texture_id != 0) {
        const std::string_view texture =
            assets_ ? assets_->ui_texture_name(ui.texture_id) : std::string_view();
        if (texture.empty()) {
            named         = false;
            draw.identity = FrameTapIdentity::COUNT_HASH;
        } else {
            draw.materials.emplace_back(texture);
        }
    }

    draw.instance    = kFrameTapUiInstanceBase + ui.instance;
    draw.world       = ui_rect_world(ui);
    draw.screen_bbox = {ui.x, ui.y, ui.w, ui.h};
    // depth_order は契約どおり提出順 (0 = 最初に描いた = 最背面)。
    draw.order_key   = static_cast<float>(ui_sequence_++);
    // UI は遮蔽クエリの対象外。 見えたかは受信側が pass / clip / alpha で決める。
    draw.visibility  = FrameTapVisibility::UNKNOWN;
    draw.has_alpha   = true;
    draw.alpha       = std::clamp(ui.alpha, 0.0f, 1.0f);
    draw.has_clip    = ui.has_clip;
    draw.clip        = ui.clip;

    if (ui.role == FrameTapUiRole::BAR) {
        draw.ui_role = FrameTapUiRole::BAR;
        draw.ui_fill = ui.fill;
    } else if (ui.role == FrameTapUiRole::GLYPH && !ui.glyph.empty()) {
        draw.ui_role = FrameTapUiRole::GLYPH;
        draw.ui_glyph.assign(ui.glyph.data(), ui.glyph.size());
    }
    if (draw.ui_role != FrameTapUiRole::NONE) {
        draw.ui_element.assign(ui.element.data(), ui.element.size());
    }

    if (!assign_generation_(draw, FrameTapInstanceSpace::UI, ui.instance)) {
        reject_last_draw_(FrameTapDropReason::OTHER);
        return;
    }

    if (!named) draw.tags.emplace_back(kFrameTapTagUnnamed);
}

void FrameTap::assign_depth_order_(uint32_t pass) {
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

    // 行全体が契約を満たせないフレームは出さない (seq は使ったまま = 欠損として見える)。
    const FrameTapCamera& camera = frame_.camera;
    if (camera.viewport_width == 0 || camera.viewport_height == 0 ||
        !is_finite_matrix(camera.view) || !is_finite_matrix(camera.projection)) {
        if (!warned_frame_) {
            std::fprintf(stderr,
                         "[FrameTap] viewport が 0 / カメラ行列が有限でないフレームを捨てました "
                         "(dropped_frames() で件数を確認できます)\n");
            warned_frame_ = true;
        }
        discard_open_frame_();
        return;
    }

    for (uint32_t pass = 0; pass < frame_.pass_count; ++pass) assign_depth_order_(pass);
    encode_frame_tap_line(frame_, line_);
    frame_open_ = false;
    sink_->write_line(line_);
    ++frames_written_;
}

} // namespace pictor
