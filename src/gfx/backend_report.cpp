#include "pictor/gfx/backend_report.h"

namespace pictor::gfx {

const char* attempt_outcome_name(AttemptOutcome outcome) noexcept {
    switch (outcome) {
        case AttemptOutcome::Selected:    return "selected";
        case AttemptOutcome::NotCompiled: return "not_compiled";
        case AttemptOutcome::Unavailable: return "unavailable";
        case AttemptOutcome::InitFailed:  return "init_failed";
    }
    return "unknown";
}

const char* degradation_action_name(DegradationAction action) noexcept {
    switch (action) {
        case DegradationAction::Replaced: return "replaced";
        case DegradationAction::Omitted:  return "omitted";
        case DegradationAction::Emulated: return "emulated";
    }
    return "unknown";
}

} // namespace pictor::gfx
