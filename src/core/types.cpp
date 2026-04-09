#include "pictor/core/types.h"

// Type definitions are header-only.
// This translation unit ensures the types header compiles cleanly
// and provides a place for any future non-inline implementations.

namespace pictor {

// Static assertions verified at compile time in types.h:
// - sizeof(float4x4) == 64  (1 cache line)
// - sizeof(AABB)     == 24
// - sizeof(SortPair) == 16  (verified in radix_sort.h)

} // namespace pictor
