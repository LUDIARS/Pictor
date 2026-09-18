#pragma once

#include "pictor/core/types.h"

namespace pictor {

/// Camera for rendering
struct Camera {
    float4x4 view       = float4x4::identity();
    float4x4 projection = float4x4::identity();
    float3   position;
    Frustum  frustum;
};

} // namespace pictor
