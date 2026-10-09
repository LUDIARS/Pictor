#pragma once

/// FrameTap — フレームごとの描画リストを記録して 1 行ずつ配送先へ渡す
/// (spec/feature/frame-tap.md)。
///
/// 既定 OFF (配送先なし)。 OFF の間は呼び出し側が `is_enabled()` の分岐 1 つで
/// 全体を飛ばす前提で、 各メソッドも OFF なら何もしない。 ON でも GPU は触らず、
/// CPU 側で受け取った値を記録するだけ。
///
/// 1 フレームの流れ: begin_frame → add_scene_draw / add_ui_draw (任意回) → end_frame。
/// draw の slot 配列と出力行のバッファは使い回す (確保は ON の間だけ起きる)。

#include "pictor/tap/frame_tap_asset_source.h"
#include "pictor/tap/frame_tap_sink.h"
#include "pictor/tap/frame_tap_types.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace pictor {

class FrameTap {
public:
    FrameTap() = default;
    FrameTap(const FrameTap&) = delete;
    FrameTap& operator=(const FrameTap&) = delete;

    /// 配送先を差し替える。 null は無効化と同じ。 開いていたフレームは捨てる。
    void set_sink(std::unique_ptr<IFrameTapSink> sink);
    void disable() { set_sink(nullptr); }
    bool is_enabled() const { return sink_ != nullptr; }

    /// 名前の問い合わせ先 (借用)。 null なら全 draw が指紋 ID + unnamed になる。
    void set_asset_source(const IFrameTapAssetSource* source) { assets_ = source; }

    /// フレームを開く。 開いたままのフレームがあれば出さずに捨てる。
    void begin_frame(uint64_t frame, double t, const FrameTapCamera& camera);

    /// 開いたフレームへ scene / ui の draw を 1 つ足す。 フレームが開いていなければ
    /// 捨てて `dropped_draws()` に数える (初回だけ stderr に警告)。
    void add_scene_draw(const FrameTapSceneObject& object);
    void add_ui_draw(const FrameTapUiDraw& draw);

    /// depth_order を確定し、 1 行にして配送先へ渡してフレームを閉じる。
    void end_frame();

    bool     is_frame_open()  const { return frame_open_; }
    uint64_t frames_written() const { return frames_written_; }
    uint64_t dropped_draws()  const { return dropped_draws_; }

private:
    /// 開いたフレームなら true。 閉じていれば dropped を数えて false。
    bool accept_draw_();
    FrameTapDraw& next_draw_(FrameTapPass pass);
    /// pass ごとに order_key の小さい順 (同値は記録順) に depth_order を振る。
    void assign_depth_order_(FrameTapPass pass);

    std::unique_ptr<IFrameTapSink> sink_;
    const IFrameTapAssetSource*    assets_ = nullptr;

    FrameTapFrame         frame_;
    float4x4              view_proj_ = float4x4::identity();
    std::string           line_;
    std::vector<uint32_t> order_scratch_;

    bool     frame_open_     = false;
    uint32_t ui_sequence_    = 0;
    uint64_t frames_written_ = 0;
    uint64_t dropped_draws_  = 0;
    bool     warned_dropped_ = false;
};

} // namespace pictor
