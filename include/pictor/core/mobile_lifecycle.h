#pragma once

#include <cstdint>
#include <string>

namespace pictor {

// ============================================================
// Mobile Lifecycle
// ============================================================
// Public surface for Android / iOS hosts to drive Pictor through
// platform-imposed lifecycle transitions. Pictor itself stays
// platform-agnostic — the host JNI / Objective-C layer maps
// platform callbacks (onPause / applicationWillResignActive /
// surface-lost / onTrimMemory / onThermalStatusChanged) onto the
// methods on PictorRenderer.
//
// The API is deliberately narrow: Pictor records the state
// transition and suspends / resumes frame work; callers retain
// responsibility for platform-specific swap-chain recreation.

// ------------------------------------------------------------
// Thermal state. Mirrors Android PowerManager.THERMAL_STATUS_*
// and iOS ProcessInfo.thermalState so hosts can forward without
// translation.
// ------------------------------------------------------------
enum class ThermalState : uint8_t {
    NOMINAL  = 0,  // cold / fair → full-throttle
    FAIR     = 1,  // slight warming
    SERIOUS  = 2,  // throttled — recommend MobileLow
    CRITICAL = 3,  // near-emergency — skip frames or drop further
    EMERGENCY = 4, // shutdown imminent — caller should unwind
};

/// Foreground / background tracking. `PAUSED` includes app-
/// switcher-visible; `SUSPENDED` is the stricter iOS background
/// state where no GPU submission may happen. `SURFACE_LOST`
/// signals the host that the swap chain / surface is invalid
/// (Android onSurfaceDestroyed or iOS view backgrounded).
enum class LifecycleState : uint8_t {
    ACTIVE        = 0,  // normal rendering
    PAUSED        = 1,  // app in app-switcher / visible but inactive
    SUSPENDED     = 2,  // strictly backgrounded — no GPU work
    SURFACE_LOST  = 3,  // surface/swap-chain gone, awaiting regain
};

/// Memory-pressure level reported by the platform (Android
/// onTrimMemory → TRIM_MEMORY_* buckets; iOS didReceiveMemoryWarning).
enum class MemoryPressure : uint8_t {
    NORMAL     = 0,
    MODERATE   = 1,  // hint: drop warm caches if convenient
    CRITICAL   = 2,  // hint: drop everything reclaimable
};

// ------------------------------------------------------------
// Auto-downgrade policy. When `enabled`, PictorRenderer will
// automatically call set_profile(low_profile_name) when the
// thermal state crosses `downgrade_at` (SERIOUS by default) and
// restore `high_profile_name` when it falls back to NOMINAL or
// FAIR. Callers can also listen via the observer and make their
// own decision.
// ------------------------------------------------------------
struct MobileAutoDowngradePolicy {
    bool         enabled             = false;
    std::string  low_profile_name    = "MobileLow";
    std::string  high_profile_name   = "MobileHigh";
    ThermalState downgrade_at        = ThermalState::SERIOUS;
    /// Below this level, restore the high profile.
    ThermalState restore_below       = ThermalState::FAIR;
};

// ------------------------------------------------------------
// Observer that hosts / sub-systems can install to react to
// lifecycle transitions. All callbacks execute on the thread
// that drove the lifecycle change — typically the UI thread on
// Android and the main thread on iOS.
// ------------------------------------------------------------
class IMobileLifecycleObserver {
public:
    virtual ~IMobileLifecycleObserver() = default;

    /// Lifecycle transition. `prev` / `next` are adjacent states.
    virtual void on_lifecycle_change(LifecycleState prev, LifecycleState next) = 0;

    /// Thermal transition.
    virtual void on_thermal_change(ThermalState prev, ThermalState next) = 0;

    /// Memory pressure — cheap caches should flush on MODERATE,
    /// everything reclaimable on CRITICAL.
    virtual void on_memory_pressure(MemoryPressure level) = 0;
};

// ------------------------------------------------------------
// Current snapshot. Queryable on the renderer so hosts can read
// it back (e.g. to log analytics or to decide whether to submit
// a heavy compute job).
// ------------------------------------------------------------
struct MobileLifecycleSnapshot {
    LifecycleState  lifecycle   = LifecycleState::ACTIVE;
    ThermalState    thermal     = ThermalState::NOMINAL;
    MemoryPressure  memory      = MemoryPressure::NORMAL;
    /// Frame where the most recent lifecycle transition occurred.
    /// 0 if no transition has happened yet.
    uint64_t        last_change_frame = 0;
};

} // namespace pictor
