// CPU blendshape application for the FBX viewer (SPEC-PC-FBX-TRACK-PLAYBACK).
//
// Shapes are stored as sparse (vertex, delta) lists built once at load
// time. Every vertex any shape touches is a "slot": its original position is
// kept, and each frame the slot is re-based from it before the weighted
// deltas are added, so repeated frames never accumulate drift.
//
//   position = base_position + Σ weight_k * delta_k   (weight_k > 0)
//
// Output goes to a strided float array (the host-visible vertex buffer), so
// the applier does not depend on the vertex layout. Normals are unchanged.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace pictor_fbx_viewer {

/// Flat, load-time-built shape data. Same-named shapes on several
/// geometries collapse into one channel.
struct MorphTargets {
    std::vector<std::string> names;          // one per channel
    std::vector<uint32_t>    shape_begin;    // channel → first entry, size names+1
    std::vector<uint32_t>    entry_slot;     // entry → slot
    std::vector<float>       entry_delta;    // entry → xyz
    std::vector<uint32_t>    slot_vertex;    // slot → vertex index in the mesh
    std::vector<float>       slot_base;      // slot → original xyz

    uint32_t channel_count() const { return static_cast<uint32_t>(names.size()); }
    bool empty() const { return names.empty(); }
};

/// Accumulates dense per-mesh deltas into MorphTargets.
class MorphTargetsBuilder {
public:
    /// `deltas` holds vertex_count * 3 floats for vertices
    /// [vertex_base, vertex_base + vertex_count); `positions` is the full
    /// mesh position array with `stride_floats` between vertices.
    void add_shape(const std::string& name, uint32_t vertex_base, uint32_t vertex_count,
                   const float* deltas, const float* positions, size_t stride_floats);
    MorphTargets build();

private:
    struct Pending { std::vector<uint32_t> slot; std::vector<float> delta; };
    std::vector<std::string> names_;
    std::vector<Pending>     shapes_;
    std::vector<uint32_t>    slot_vertex_;
    std::vector<float>       slot_base_;
    std::unordered_map<uint32_t, uint32_t> slot_of_vertex_;  // load-time only
    uint32_t slot_for(uint32_t vertex, const float* positions, size_t stride_floats);
};

/// Applies channel weights into a strided position array. Owns a scratch
/// buffer sized once, so apply() does not allocate.
class MorphApplier {
public:
    void bind(const MorphTargets* targets);
    /// `weights` has targets->channel_count() entries.
    void apply(const float* weights, float* positions, size_t stride_floats);

private:
    const MorphTargets* targets_ = nullptr;
    std::vector<float>  accum_;
};

} // namespace pictor_fbx_viewer
