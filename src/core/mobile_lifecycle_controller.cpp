#include "pictor/core/mobile_lifecycle_controller.h"

namespace pictor {

void MobileLifecycleController::transition_lifecycle_(LifecycleState next) {
    if (snapshot_.lifecycle == next) return;
    LifecycleState prev = snapshot_.lifecycle;
    snapshot_.lifecycle = next;
    snapshot_.last_change_frame = hooks_.current_frame ? hooks_.current_frame() : 0;
    if (observer_) {
        observer_->on_lifecycle_change(prev, next);
    }
    // resume 時は frame allocator をフラッシュし、 pause 前フレームの stale ポインタが
    // 最初の active フレームへ漏れないようにする (end_frame は suppress 中スキップ済)。
    if (prev != LifecycleState::ACTIVE && next == LifecycleState::ACTIVE &&
        hooks_.flush_frame_allocator) {
        hooks_.flush_frame_allocator();
    }
}

void MobileLifecycleController::transition_thermal_(ThermalState next) {
    if (snapshot_.thermal == next) return;
    ThermalState prev = snapshot_.thermal;
    snapshot_.thermal = next;
    snapshot_.last_change_frame = hooks_.current_frame ? hooks_.current_frame() : 0;
    if (observer_) {
        observer_->on_thermal_change(prev, next);
    }
    // Auto-downgrade: opt-in 時のみ、 閾値を跨いだらプロファイルを入れ替える。
    if (!policy_.enabled) return;

    const bool hot_enough  = static_cast<uint8_t>(next) >= static_cast<uint8_t>(policy_.downgrade_at);
    const bool cool_enough = static_cast<uint8_t>(next) <= static_cast<uint8_t>(policy_.restore_below);
    // 判定は downgraded_ で行う。 pre_downgrade_profile_ の空文字を sentinel に
    // すると active_profile() が空を返す host で downgrade が毎回再発火し、
    // restore が永久に走らない。
    if (hot_enough && !downgraded_) {
        pre_downgrade_profile_ = hooks_.active_profile ? hooks_.active_profile() : std::string{};
        if (!policy_.low_profile_name.empty() && hooks_.switch_profile) {
            hooks_.switch_profile(policy_.low_profile_name);
        }
        downgraded_ = true;
    } else if (cool_enough && downgraded_) {
        const std::string restore = policy_.high_profile_name.empty()
            ? pre_downgrade_profile_
            : policy_.high_profile_name;
        if (!restore.empty() && hooks_.switch_profile) hooks_.switch_profile(restore);
        pre_downgrade_profile_.clear();
        downgraded_ = false;
    }
}

void MobileLifecycleController::on_pause() {
    if (snapshot_.lifecycle == LifecycleState::SURFACE_LOST) return; // stricter wins
    transition_lifecycle_(LifecycleState::PAUSED);
}

void MobileLifecycleController::on_resume() {
    if (snapshot_.lifecycle == LifecycleState::SURFACE_LOST) return; // wait for regain
    transition_lifecycle_(LifecycleState::ACTIVE);
}

void MobileLifecycleController::on_suspend() {
    transition_lifecycle_(LifecycleState::SUSPENDED);
}

void MobileLifecycleController::on_surface_lost() {
    transition_lifecycle_(LifecycleState::SURFACE_LOST);
}

void MobileLifecycleController::on_surface_regained() {
    if (snapshot_.lifecycle != LifecycleState::SURFACE_LOST) return;
    transition_lifecycle_(LifecycleState::ACTIVE);
}

void MobileLifecycleController::on_low_memory(MemoryPressure level) {
    if (snapshot_.memory == level) return;
    snapshot_.memory = level;
    snapshot_.last_change_frame = hooks_.current_frame ? hooks_.current_frame() : 0;
    if (observer_) {
        observer_->on_memory_pressure(level);
    }
}

void MobileLifecycleController::on_thermal_state(ThermalState state) {
    transition_thermal_(state);
}

} // namespace pictor
