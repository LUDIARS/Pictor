#pragma once
#include "pn_model.h"
#include "pictor/demo/polynomial_motion.h"
#include <array>
#include <vector>

namespace pictor_kuzuha {
// std430 vec4 layout shared with pn_ray_common.glsl. Immutable after upload.
struct alignas(16) GpuPnPatch {
    std::array<std::array<float,4>,10> control{};
    std::array<std::array<float,4>,3> normals{};
    std::array<float,4> uv01{}, uv2_bounds{}, lower{}, upper{};
};
static_assert(sizeof(GpuPnPatch)==272);
std::vector<GpuPnPatch> make_gpu_patches(const PnModel& model);
GpuPnPatch make_gpu_patch(const pictor::demo::PolynomialPatch& patch);
}
