#pragma once
#include "pictor/core/types.h"
#include <array>

namespace pictor_kuzuha {
using Vec = pictor::float3;
struct PnPatch {
    // Cubic triangular Bernstein coefficients: 300,030,003,210,120,021,012,102,201,111.
    std::array<Vec, 10> control;
};
PnPatch make_patch(const std::array<Vec, 3>& position,
                   const std::array<Vec, 6>& edge_normals,
                   const std::array<bool, 3>& straight_edge);
Vec evaluate(const PnPatch& patch, float u, float v, float w);
Vec unit(Vec value);
}
