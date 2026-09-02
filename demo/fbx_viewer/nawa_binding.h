// Rope-binding ("nawa") deformation for the FBX viewer.
//
// SDF capsule vertex displacement: a rope is modelled as a chain of
// capsules laid on a
// cylindrical "bind frame" around the body; every vertex samples the
// signed distance to the chain and sinks under the rope (groove) while
// the flesh beside it bulges (ring). Fur shells additionally crush their
// extrusion inside the groove.
//
// CPU side (this file): pattern curves -> capsule chain -> shader state.
// GPU side: shaders/nawa_binding_common.glsl (set 0, binding 3 UBO).
#pragma once

#include "pictor/core/types.h"

#include <cstdint>
#include <functional>
#include <vector>

namespace pictor_fbx_viewer {

// The viewer's UBO uses enough segments for spirals to stay round. Must
// match the GLSL UBO.
constexpr uint32_t kNawaMaxSegments = 96;

struct BindingFrame;
struct BindingBodyProfile;

enum class BindingPatternKind : uint32_t {
    Guruguru = 0,   // single spiral
    Kikkou   = 1,   // two counter-wound spirals (diamond lattice)
    Tasuki   = 2,   // two half-turn strands crossing at the middle
    Obi      = 3,   // three touching loops around the waist: only the middle pinches in
};

const char* binding_pattern_name(BindingPatternKind kind);
bool parse_binding_pattern(const char* text, BindingPatternKind& out);

/// Pattern in bind-frame cylindrical coordinates (body axis t, angle theta).
struct BindingPatternSpec {
    BindingPatternKind kind = BindingPatternKind::Obi;
    float turns = 3.0f;
    float theta_offset_rad = 0.0f;
    float body_start = 0.15f;
    float body_end   = 0.85f;
    /// Obi only: axis spacing (in t) between the parallel loops, set by
    /// limit_turns_for so neighbouring ropes touch.
    float strand_spacing_t = 0.0f;

    static BindingPatternSpec create_default(BindingPatternKind kind);

    /// Clamp `turns` so neighbouring passes of the rope keep their grooves
    /// apart: pitch >= k * (rope radius + groove width), k = 1.5 for a
    /// single strand, 2.2 for two crossing strands. Returns the spec.
    BindingPatternSpec& limit_turns_for(const BindingFrame& frame,
                                        const BindingBodyProfile& profile);
};

/// Capsule approximation of the body axis (model space).
struct BindingFrame {
    pictor::float3 start{};
    pictor::float3 end{};
    float radius_start = 1.0f;
    float radius_end   = 1.0f;
    pictor::float3 theta_zero_dir{0.0f, 0.0f, 1.0f};
};

/// Deformation profile in model units (preset values are in metres; scale
/// with `scaled_plush`).
struct BindingBodyProfile {
    float sink_depth   = 0.016f;
    float bulge_height = 0.007f;
    float groove_width = 0.022f;
    float bulge_width  = 0.030f;
    float fur_crush    = 0.85f;
    float seam_emphasis = 0.6f;
    float rope_radius  = 0.012f;
    float surface_embed_ratio = 0.45f;
    /// Sink the rope centre by the groove depth so the tube rests on the
    /// dented surface instead of floating above it.
    bool  rope_follows_dent = false;

    /// Plush preset multiplied into model units.
    static BindingBodyProfile scaled_plush(float metres_to_units);

    /// Widen/deepen the groove for a single waist loop so the whole middle
    /// reads as cinched rather than a thin scratch.
    BindingBodyProfile pinched_for(BindingPatternKind kind) const;
};

struct NawaCapsuleSegment {
    pictor::float3 start{};
    pictor::float3 end{};
    float radius = 0.0f;
};

struct NawaCapsuleChain {
    std::vector<NawaCapsuleSegment>            segments;   // <= kNawaMaxSegments
    std::vector<std::vector<pictor::float3>>   strands;    // polyline per strand (for rope geometry)
};

/// Std140 layout of the GLSL `NawaBindUBO`.
struct NawaBindUBO {
    float seg_start[kNawaMaxSegments][4];   // xyz start, w rope radius
    float seg_end[kNawaMaxSegments][4];     // xyz end
    float deform[4];    // sink depth, bulge height, groove width, bulge width
    float surface[4];   // fur crush, seam emphasis, unused, smooth-min blend
    float meta[4];      // x = active segment count
};
static_assert(sizeof(NawaBindUBO) == (kNawaMaxSegments * 2 + 3) * 16,
              "NawaBindUBO must match the std140 block in nawa_binding_common.glsl");

/// Sample (t, theta) on a strand. Returns {t, theta}.
void sample_binding_strand(const BindingPatternSpec& spec, int strand, float s,
                           float& t_out, float& theta_out);
int  binding_strand_count(BindingPatternKind kind);

/// Point on the frame surface lifted by `radial_offset`.
pictor::float3 binding_surface_point(const BindingFrame& frame, float t, float theta,
                                     float radial_offset);

/// Optional body-surface radius override at (t, theta). Lets the chain
/// hug a real mesh instead of the linear capsule approximation.
using SurfaceRadiusFn = std::function<float(float t, float theta)>;

/// Build the capsule chain for a pattern on a frame (fills up to
/// kNawaMaxSegments, split evenly across strands).
NawaCapsuleChain build_capsule_chain(const BindingPatternSpec& spec,
                                     const BindingFrame& frame,
                                     const BindingBodyProfile& profile,
                                     const SurfaceRadiusFn& surface_radius = nullptr);

/// Surface radius from a triangle mesh: casts a ray from the frame axis
/// outward at (t, theta) and returns the distance to the nearest hit, i.e.
/// the body skin (appendages such as wings lie beyond it and are ignored).
/// Falls back to the frame radius when nothing is hit. Copies the mesh.
SurfaceRadiusFn make_mesh_surface_radius(const pictor::float3* positions, size_t vertex_count,
                                         const uint32_t* indices, size_t index_count,
                                         const BindingFrame& frame);

/// Compose the UBO for the given tightness (0..1). `visible_segments`
/// caps how much of the chain is active (wrap animation); pass the
/// full count for a finished tie.
NawaBindUBO compose_bind_ubo(const NawaCapsuleChain& chain,
                             const BindingBodyProfile& profile,
                             float tightness,
                             uint32_t visible_segments);

/// Estimate a vertical bind frame from a vertex cloud: axis through the
/// bounds centre, radii from a horizontal-distance percentile near each
/// end (robust against thin appendages such as wings).
BindingFrame estimate_binding_frame(const pictor::float3* positions, size_t count,
                                    float body_start_ratio = 0.22f,
                                    float body_end_ratio   = 0.78f);

} // namespace pictor_fbx_viewer
