// External track file for the FBX viewer (`--track <file.csv>`,
// SPEC-PC-FBX-TRACK-PLAYBACK).
//
// The CSV is parsed once at start-up into flat, frame-major arrays so the
// per-frame player only indexes memory (no allocation, no string work).
// Header columns:
//   frame                       0, 1, 2 ... without gaps
//   bone:<BoneName>.rx|ry|rz    degrees on top of the bind-pose local rotation
//   morph:<ShapeName>           blendshape weight (1 = full shape)
// Unknown bone / shape names are ignored with one warning each. Fields are
// plain decimal numbers; quoting is not supported.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace pictor_fbx_viewer {

struct TrackData {
    uint32_t frame_count = 0;

    // Bone channel groups (one per targeted bone, axes merged).
    std::vector<uint32_t> bone_index;      // index into the skeleton
    std::vector<float>    bone_euler_deg;  // [frame][group][xyz], frame-major

    // Morph channels (one per targeted shape).
    std::vector<uint32_t> morph_index;     // index into the shape name list
    std::vector<float>    morph_weight;    // [frame][channel], frame-major

    // Non-fatal diagnostics (unknown names), one entry per name.
    std::vector<std::string> warnings;

    uint32_t bone_group_count() const { return static_cast<uint32_t>(bone_index.size()); }
    uint32_t morph_count() const { return static_cast<uint32_t>(morph_index.size()); }
    const float* bone_frame(uint32_t frame) const {
        return bone_euler_deg.data() + static_cast<size_t>(frame) * bone_index.size() * 3;
    }
    const float* morph_frame(uint32_t frame) const {
        return morph_weight.data() + static_cast<size_t>(frame) * morph_index.size();
    }
};

/// Parse CSV text against the model's bone and shape names. Returns false
/// with `error` set on malformed input or when no column targets a known
/// channel; `out` is left in an unspecified state on failure.
bool parse_track_csv(std::string_view text,
                     const std::vector<std::string>& bone_names,
                     const std::vector<std::string>& shape_names,
                     TrackData& out, std::string& error);

/// Read `path` and parse it with parse_track_csv().
bool load_track_csv(const std::string& path,
                    const std::vector<std::string>& bone_names,
                    const std::vector<std::string>& shape_names,
                    TrackData& out, std::string& error);

} // namespace pictor_fbx_viewer
