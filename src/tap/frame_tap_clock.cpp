#include "pictor/tap/frame_tap_clock.h"

#include <cmath>

namespace pictor {

double FrameTapClock::next_time(double fallback_seconds) {
    const double candidate = has_host_time_ ? host_time_ : fallback_seconds;
    // 有限でない・負・逆行は前の値に留める (契約: t は 0 以上で減らない)。
    if (std::isfinite(candidate) && candidate > last_time_) last_time_ = candidate;
    return last_time_;
}

bool FrameTapClock::next_tick(uint64_t& out) {
    if (!has_host_tick_) return false;
    if (host_tick_ > last_tick_) last_tick_ = host_tick_;
    out = last_tick_;
    return true;
}

} // namespace pictor
