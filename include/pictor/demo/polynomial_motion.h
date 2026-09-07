#pragma once
#include "pictor/core/types.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace pictor::demo {
struct MotionVertex {
    float3 position{},normal{};
    std::array<uint32_t,4> joints{};
    std::array<float,4> weights{};
};
struct MotionBone { std::string name;int32_t parent=-1;float3 position{}; };
struct PolynomialPatch {
    // Bernstein order: 300,030,003,210,120,021,012,102,201,111.
    std::array<float3,10> control{};
    std::array<float3,3> normals{};
    std::array<std::array<float,2>,3> uv{};
    bool constant_color=false;
    float3 color{1,1,1};
};
enum class MotionCommand { zero_force,positive_force,negative_force,pause,reset };

// Optional demo boundary. Pictor consumes polynomial state and never depends on
// the implementation that owns its physics. All input views live for initialize().
class PolynomialMotion {
public:
    virtual ~PolynomialMotion()=default;
    virtual void initialize(std::span<const MotionVertex> vertices,
        std::span<const uint32_t> indices,std::span<const MotionBone> bones,
        std::span<const PolynomialPatch> source)=0;
    // Storage and patch count remain fixed after initialize. Additional patches
    // follow the source patches and use an opaque constant color.
    virtual std::span<const PolynomialPatch> patches() const=0;
    virtual std::span<const uint32_t> changed_patches() const=0;
    virtual void update(double real_seconds)=0;
    virtual void command(MotionCommand command)=0;
    virtual void status(char* destination,size_t capacity) const=0;
    virtual void record_capture() const=0;
};
// Blocking native viewer. The caller owns motion until the function returns.
// Available through the optional installed PictorDemo package.
int run_polynomial_viewer(int argc,char** argv,PolynomialMotion* motion);
} // namespace pictor::demo
