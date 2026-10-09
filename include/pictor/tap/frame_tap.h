#pragma once

/// FrameTap — フレームごとの描画リストを記録して 1 行ずつ配送先へ渡す
/// (spec/feature/frame-tap.md、 契約 render-tap/1)。
///
/// 既定 OFF (配送先なし)。 OFF の間は呼び出し側が `is_enabled()` の分岐 1 つで
/// 全体を飛ばす前提で、 各メソッドも OFF なら何もしない。 ON でも GPU は触らず、
/// CPU 側で受け取った値を記録するだけ。
///
/// 1 フレームの流れ: begin_frame → (add_pass) → add_scene_draw / add_ui_draw (任意回)
/// → end_frame。 配送先を外す (set_sink / disable / end_stream / 破棄) ときは end 行
/// を出して流れを閉じる。 draw の slot 配列と出力行のバッファは使い回す
/// (確保は ON の間だけ起きる)。

#include "pictor/tap/frame_tap_asset_source.h"
#include "pictor/tap/frame_tap_clock.h"
#include "pictor/tap/frame_tap_generations.h"
#include "pictor/tap/frame_tap_sink.h"
#include "pictor/tap/frame_tap_types.h"
#include "pictor/tap/frame_tap_visibility_source.h"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace pictor {

class FrameTap {
public:
    /// add_pass が失敗したときの値。
    static constexpr uint32_t kInvalidPass = UINT32_MAX;

    FrameTap() = default;
    ~FrameTap();
    FrameTap(const FrameTap&) = delete;
    FrameTap& operator=(const FrameTap&) = delete;

    /// 配送先を差し替える。 null は無効化と同じ。 前の配送先があれば開いていた
    /// フレームを捨て、 end 行 (shutdown) を出してから外す。 新しい流れの seq は 0 から。
    /// コールバック配送先は、 その参照先がタップより長く生きるか、 先に disable すること。
    void set_sink(std::unique_ptr<IFrameTapSink> sink);
    void disable() { set_sink(nullptr); }
    bool is_enabled() const { return sink_ != nullptr; }

    /// end 行を出して流れを閉じ、 無効化する (`FAILED` はホスト側の異常終了)。
    void end_stream(FrameTapEndReason reason);

    /// 名前の問い合わせ先 (借用)。 null なら全 draw が count-hash + unnamed になる。
    void set_asset_source(const IFrameTapAssetSource* source) { assets_ = source; }

    /// scene draw の可視性の根拠 (借用)。 null なら frustum-only で lag を出さない。
    void set_visibility_source(const IFrameTapVisibilitySource* source) { visibility_ = source; }
    const IFrameTapVisibilitySource* visibility_source() const { return visibility_; }

    /// 観測者 ID (`observer.id`)。 既定は "player-camera"。 書式 `^[a-z0-9][a-z0-9_.-]*$`
    /// に合わなければ false で変えない。
    bool set_observer_id(std::string_view id);
    const std::string& observer_id() const { return observer_id_; }

    /// ゲーム時刻 / tick (FrameTapClock)。 与えなければ begin_frame の fallback_t を使う。
    FrameTapClock&       clock()       { return clock_; }
    const FrameTapClock& clock() const { return clock_; }

    /// 1 フレームに記録する draw の上限 (0 = 無制限、 既定)。 超えた分は dropped
    /// (buffer-full) に数える。
    void set_max_draws_per_frame(uint32_t max_draws) { max_draws_ = max_draws; }

    /// フレームを開く (seq を 1 つ使う)。 開いたままのフレームがあれば出さずに捨てる。
    /// `fallback_t` はホストがゲーム時刻を与えていないときの `t`。 scene / ui の 2 pass を開く。
    void begin_frame(uint64_t frame, double fallback_t, const FrameTapCamera& camera);

    /// 開いたフレームへ pass を足して添字を返す (出力順 = 足した順、 scene / ui の後)。
    /// フレームが開いていない / name が空なら kInvalidPass。
    uint32_t add_pass(std::string_view name, FrameTapPassKind kind);

    /// 開いたフレームへ draw を 1 つ足す (object.pass / draw.pass の pass へ)。 フレームが
    /// 開いていなければ捨てて `dropped_draws()` に数える (初回だけ stderr に警告)。
    /// 数値が有限でない・契約の範囲外の draw は出さずにそのフレームの dropped (other) に数える。
    void add_scene_draw(const FrameTapSceneObject& object);
    void add_ui_draw(const FrameTapUiDraw& draw);

    /// depth_order を確定し、 1 行にして配送先へ渡してフレームを閉じる。 カメラ行列が
    /// 有限でない / viewport が 0 のフレームは出さない (seq は使ったまま = 受信側では欠損)。
    void end_frame();

    bool     is_frame_open()   const { return frame_open_; }
    uint64_t frames_written()  const { return frames_written_; }
    /// 出さずに捨てたフレームの数 (seq の飛びになる)。
    uint64_t dropped_frames()  const { return dropped_frames_; }
    /// 捨てた draw の総数 (フレーム外に届いたもの + 各フレームの dropped)。
    uint64_t dropped_draws()   const { return dropped_draws_; }

private:
    bool accept_draw_();
    /// draw slot を 1 つ確保する。 上限に達していれば dropped (buffer-full) で null。
    FrameTapDraw* next_draw_(uint32_t pass);
    /// 直前に確保した slot を取り消し、 dropped (reason) に数える。
    void reject_last_draw_(FrameTapDropReason reason);
    void count_frame_drop_(FrameTapDropReason reason);
    /// 外見から世代を付ける。 追跡できなければ false。
    bool assign_generation_(FrameTapDraw& draw, FrameTapInstanceSpace space, uint32_t key);
    /// pass ごとに order_key の小さい順 (同値は記録順) に depth_order を振る。
    void assign_depth_order_(uint32_t pass);
    void discard_open_frame_();
    void write_end_line_(FrameTapEndReason reason);

    std::unique_ptr<IFrameTapSink>   sink_;
    const IFrameTapAssetSource*      assets_     = nullptr;
    const IFrameTapVisibilitySource* visibility_ = nullptr;

    std::string         observer_id_ = kFrameTapPlayerObserver;
    FrameTapClock       clock_;
    FrameTapGenerations generations_;

    FrameTapFrame         frame_;
    float4x4              view_proj_ = float4x4::identity();
    std::string           line_;
    std::vector<uint32_t> order_scratch_;
    std::vector<uint64_t> appearance_scratch_;

    uint32_t max_draws_      = 0;
    bool     frame_open_     = false;
    uint32_t ui_sequence_    = 0;
    uint64_t next_seq_       = 0;
    uint64_t frames_written_ = 0;
    uint64_t dropped_frames_ = 0;
    uint64_t dropped_draws_  = 0;
    bool     warned_dropped_ = false;
    bool     warned_frame_   = false;
};

} // namespace pictor
