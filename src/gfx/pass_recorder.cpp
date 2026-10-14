#include "pictor/gfx/pass_recorder.h"

namespace pictor::gfx {

const char* record_error_name(RecordError e) noexcept {
    switch (e) {
        case RecordError::Ok:                  return "ok";
        case RecordError::InvalidHandle:       return "invalid_handle";
        case RecordError::BindGroupOutOfRange: return "bind_group_out_of_range";
        case RecordError::ComputeUnavailable:  return "compute_unavailable";
    }
    return "unknown";
}

} // namespace pictor::gfx
