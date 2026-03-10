#include "pictor/scene/soa_stream.h"

// SoAStream is fully template-based (header-only).
// This translation unit ensures the header compiles cleanly.

namespace pictor {

// Explicit instantiations for common types to reduce compile times
template class SoAStream<float>;
template class SoAStream<uint8_t>;
template class SoAStream<uint16_t>;
template class SoAStream<uint32_t>;
template class SoAStream<uint64_t>;

} // namespace pictor
