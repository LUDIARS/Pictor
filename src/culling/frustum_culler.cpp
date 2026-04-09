#include "pictor/culling/culling_system.h"
#include <cmath>

// Frustum culling helper functions.
// The actual culling is inline in CullingSystem::frustum_cull_pool().
// This file provides utility functions for frustum extraction.

namespace pictor {

namespace frustum_utils {

/// Extract frustum planes from a view-projection matrix
Frustum extract_frustum(const float4x4& vp) {
    Frustum f;

    // Left plane
    f.planes[0].normal.x = vp.m[0][3] + vp.m[0][0];
    f.planes[0].normal.y = vp.m[1][3] + vp.m[1][0];
    f.planes[0].normal.z = vp.m[2][3] + vp.m[2][0];
    f.planes[0].distance = vp.m[3][3] + vp.m[3][0];

    // Right plane
    f.planes[1].normal.x = vp.m[0][3] - vp.m[0][0];
    f.planes[1].normal.y = vp.m[1][3] - vp.m[1][0];
    f.planes[1].normal.z = vp.m[2][3] - vp.m[2][0];
    f.planes[1].distance = vp.m[3][3] - vp.m[3][0];

    // Bottom plane
    f.planes[2].normal.x = vp.m[0][3] + vp.m[0][1];
    f.planes[2].normal.y = vp.m[1][3] + vp.m[1][1];
    f.planes[2].normal.z = vp.m[2][3] + vp.m[2][1];
    f.planes[2].distance = vp.m[3][3] + vp.m[3][1];

    // Top plane
    f.planes[3].normal.x = vp.m[0][3] - vp.m[0][1];
    f.planes[3].normal.y = vp.m[1][3] - vp.m[1][1];
    f.planes[3].normal.z = vp.m[2][3] - vp.m[2][1];
    f.planes[3].distance = vp.m[3][3] - vp.m[3][1];

    // Near plane
    f.planes[4].normal.x = vp.m[0][2];
    f.planes[4].normal.y = vp.m[1][2];
    f.planes[4].normal.z = vp.m[2][2];
    f.planes[4].distance = vp.m[3][2];

    // Far plane
    f.planes[5].normal.x = vp.m[0][3] - vp.m[0][2];
    f.planes[5].normal.y = vp.m[1][3] - vp.m[1][2];
    f.planes[5].normal.z = vp.m[2][3] - vp.m[2][2];
    f.planes[5].distance = vp.m[3][3] - vp.m[3][2];

    // Normalize all planes
    for (int i = 0; i < 6; ++i) {
        float len = std::sqrt(f.planes[i].normal.x * f.planes[i].normal.x +
                              f.planes[i].normal.y * f.planes[i].normal.y +
                              f.planes[i].normal.z * f.planes[i].normal.z);
        if (len > 0.0f) {
            float inv = 1.0f / len;
            f.planes[i].normal.x *= inv;
            f.planes[i].normal.y *= inv;
            f.planes[i].normal.z *= inv;
            f.planes[i].distance *= inv;
        }
    }

    return f;
}

} // namespace frustum_utils

} // namespace pictor
