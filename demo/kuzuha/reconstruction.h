#pragma once
#include "pn_model.h"
#include "raymarch_pass.h"
#include "stage_orbit.h"
#include <string>

namespace pictor_kuzuha {
struct Options {
    bool enabled=false, head=true;
    bool raymarch=true, auto_lod=true, animate_strength=false;
    bool stage=false, stage_fixed_camera=false;
    unsigned factor=4, display=0;
    unsigned width=1280, height=720;
    float strength=1, yaw=90, zoom=1;
    std::string capture_directory, report;
    std::string stage_orbit_curve;
};
// Consumes only reconstruction options; throws on missing/invalid values.
bool parse_option(int argc,char** argv,int& index,Options& options);
class Reconstruction {
public:
    Reconstruction(pictor_fbx_viewer::PackedMesh source,Options options);
    pictor_fbx_viewer::PackedMesh rebuild();
    bool key(int code);
    void update(float dt);
    void stage_time(float seconds) { stage_time_=seconds; }
    void camera_translation(pictor::float3 offset) { camera_translation_=offset; }
    RaymarchParameters ray_parameters(unsigned width,unsigned height) const;
    void record_capture(uint32_t unresolved,float frame_ms) const;
    const PnModel& model() const { return model_; }
    const Options& options() const { return options_; }
    float pixel_tolerance() const { return 1.f/options_.factor; }
    /// @implements SPEC-PC-KUZUHA-PN
    bool dirty() const { return dirty_; }
    void camera(float eye[3],float center[3]) const;
    std::string title() const;
    /// @implements SPEC-PC-KUZUHA-PN
    unsigned display() const { return options_.display; }
    std::string advance_capture();
private:
    PnModel model_;
    Options options_;
    StageOrbit stage_orbit_;
    RefinementStats stats_;
    bool dirty_=true;
    unsigned capture_index_=0;
    float height_=1, min_y_=0;
    float phase_=3.14159265f;
    float stage_time_=0;
    pictor::float3 camera_translation_{};
};
std::string first_capture(Options& options);
}
