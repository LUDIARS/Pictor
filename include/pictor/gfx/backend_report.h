#pragma once

#include "pictor/gfx/backend_kind.h"
#include "pictor/gfx/device_caps.h"

#include <cstdint>
#include <string>
#include <vector>

namespace pictor::gfx {

/// Result of trying one backend during selection.
enum class AttemptOutcome : uint8_t {
    Selected = 0,   ///< this backend is the one in use
    NotCompiled,    ///< not built into this binary
    Unavailable,    ///< built, but the platform / driver does not offer it
    InitFailed,     ///< offered, but device / surface creation failed
};

/// How a missing feature was handled (declared downgrades only).
enum class DegradationAction : uint8_t {
    Replaced = 0,   ///< declared fallback used instead
    Omitted,        ///< declared `omit`: the subject is not drawn
    Emulated,       ///< same result, different implementation
};

struct BackendAttempt {
    BackendKind    kind    = BackendKind::Null;
    AttemptOutcome outcome = AttemptOutcome::NotCompiled;
    std::string    reason;
};

struct DegradationEntry {
    Feature           feature        = Feature::Compute;
    DegradationAction action         = DegradationAction::Replaced;
    uint32_t          subject_id     = 0;   ///< pass / module / Visus variant id
    uint32_t          replacement_id = 0;   ///< 0 when omitted / emulated
    std::string       reason;
};

/// Record of backend selection: which backend runs, every attempt made on the
/// way there, and every downgrade applied. Written once at init (and on
/// profile / backend switch); never read on the per-frame hot path.
struct BackendReport {
    bool                          has_selection = false;
    BackendKind                   selected      = BackendKind::Null;
    CapabilityTier                tier          = CapabilityTier::C_Full;
    bool                          fallback_used = false;  ///< a later preference was taken
    std::vector<BackendAttempt>   attempts;
    std::vector<DegradationEntry> degradations;
};

const char* attempt_outcome_name(AttemptOutcome outcome) noexcept;
const char* degradation_action_name(DegradationAction action) noexcept;

} // namespace pictor::gfx
