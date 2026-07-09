#pragma once

#include "pictor/core/mobile_lifecycle.h"
#include <functional>
#include <string>

namespace pictor {

/// モバイル lifecycle / thermal のステートマシン
/// (review/2026-06-11 D-1: PictorRenderer God Class からの切り出し)。
///
/// OS レベルのイベント (pause/resume/suspend/surface lost-regain/low-memory/
/// thermal) を受け、 lifecycle 状態・自動ダウングレードを管理する。 レンダラ本体
/// への依存は `Hooks` (フレーム番号取得 / frame allocator フラッシュ / プロファイル
/// 取得・切替) に限定し、 循環依存を避ける。
class MobileLifecycleController {
public:
    /// レンダラ本体が提供する最小限の操作。
    struct Hooks {
        std::function<uint64_t()>                  current_frame;          // frame_number
        std::function<void()>                      flush_frame_allocator;  // resume 時の stale flush
        std::function<std::string()>               active_profile;         // current_profile_name
        std::function<void(const std::string&)>    switch_profile;         // set_profile
    };

    MobileLifecycleController(Hooks hooks, MobileAutoDowngradePolicy policy)
        : hooks_(std::move(hooks)), policy_(policy) {}

    // ---- OS イベント ----
    void on_pause();
    void on_resume();
    void on_suspend();
    void on_surface_lost();
    void on_surface_regained();
    void on_low_memory(MemoryPressure level);
    void on_thermal_state(ThermalState state);

    // ---- 状態 / 設定 ----
    MobileLifecycleSnapshot snapshot() const { return snapshot_; }
    void set_observer(IMobileLifecycleObserver* observer) { observer_ = observer; }
    void set_policy(const MobileAutoDowngradePolicy& policy) { policy_ = policy; }

    /// ACTIVE 以外では per-frame 作業を抑制する (scene/handle/GPU resource は温存)。
    bool frame_work_suppressed() const {
        return snapshot_.lifecycle != LifecycleState::ACTIVE;
    }

private:
    void transition_lifecycle_(LifecycleState next);
    void transition_thermal_(ThermalState next);

    Hooks                     hooks_;
    MobileAutoDowngradePolicy policy_;
    MobileLifecycleSnapshot   snapshot_{};
    IMobileLifecycleObserver* observer_ = nullptr;
    /// 自動ダウングレード前に active だったプロファイル。
    /// 空文字はダウングレード状態の判定に使わない (active_profile() が空を
    /// 返す host でも動くよう downgraded_ で区別する)。
    std::string               pre_downgrade_profile_;
    bool                      downgraded_ = false;
};

} // namespace pictor
