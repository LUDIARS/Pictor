#pragma once

/// フレームタップの時刻 — 各行の `t` (ゲーム内時刻、 秒) と任意の `tick`
/// (spec/feature/frame-tap.md §3)。
///
/// ホストがゲーム時刻 / tick を与えればそれを使い、 与えなければ描画器の
/// 代わりの値 (PictorRenderer は begin_frame の delta_time の累積) を使う。
/// どちらの場合も出力は減らない: 前の行より小さい値・有限でない値・負の値は
/// 前の行の値に留める (受信側の time-order 違反を Pictor から出さない)。

#include <cstdint>

namespace pictor {

class FrameTapClock {
public:
    /// ホストのゲーム時刻 (秒)。 次に set / clear するまで毎フレーム使う。
    void set_game_time(double seconds) { host_time_ = seconds; has_host_time_ = true; }
    /// ホストのゲーム tick。 与えたときだけ行に `tick` が出る。
    void set_game_tick(uint64_t tick) { host_tick_ = tick; has_host_tick_ = true; }
    /// ホストの値を捨て、 代わりの値 (delta 累積) に戻す。 tick も出さなくなる。
    void clear_game_clock() { has_host_time_ = false; has_host_tick_ = false; }

    bool has_game_time() const { return has_host_time_; }
    bool has_game_tick() const { return has_host_tick_; }

    /// 新しい出力の流れ (配送先の差し替え) の始まり。 単調性の基準を 0 に戻す。
    void reset_stream() { last_time_ = 0.0; last_tick_ = 0; }

    /// このフレームの `t`。 `fallback_seconds` はホストの値が無いときに使う。
    double next_time(double fallback_seconds);

    /// このフレームの `tick` (ホストが与えていれば true)。
    bool next_tick(uint64_t& out);

private:
    double   host_time_     = 0.0;
    uint64_t host_tick_     = 0;
    bool     has_host_time_ = false;
    bool     has_host_tick_ = false;
    double   last_time_     = 0.0;
    uint64_t last_tick_     = 0;
};

} // namespace pictor
