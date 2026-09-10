#pragma once
#include "pn_gpu_data.h"
#include "raymarch_pass.h"
#include "pictor/animation/animation_types.h"
#include "pictor/profiler/bitmap_text_renderer.h"

namespace pictor_kuzuha {
/// Adapts an external PolynomialMotion implementation onto the demo's GPU
/// patch buffer. Pictor never depends on the motion implementation itself.
/// @implements SPEC-PC-POLYNOMIAL-MOTION
class MotionBridge {
public:
    void initialize(pictor::demo::PolynomialMotion* motion,const PnModel& model,
                    const pictor::SkeletonDescriptor& skeleton);
    std::vector<GpuPnPatch> initial_patches(const PnModel& model) const;
    void update(double seconds,RaymarchPass& pass);
    bool key(int code);
    void draw_status(pictor::BitmapTextRenderer& text) const;
    void record_capture() const;
    pictor::float3 camera_translation() const { return motion_?motion_->camera_translation():pictor::float3{}; }
    /// Patches appended by the motion implementation, drawn after the source.
    /// @implements SPEC-PC-POLYNOMIAL-MOTION
    uint32_t extra_count() const { return total_count_-source_count_; }
    /// @implements SPEC-PC-POLYNOMIAL-MOTION
    uint32_t source_count() const { return source_count_; }
    /// @implements SPEC-PC-POLYNOMIAL-MOTION
    bool enabled() const { return motion_!=nullptr; }
private:
    pictor::demo::PolynomialMotion* motion_=nullptr; // borrowed for viewer lifetime
    uint32_t source_count_=0,total_count_=0;
};
}
