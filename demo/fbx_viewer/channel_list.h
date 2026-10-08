// `--list-channels` output (SPEC-PC-FBX-TRACK-PLAYBACK): every bone (with
// parent and bind-pose head position in model space) and every blendshape
// name as one JSON document, so a host can build its name mapping.
#pragma once

#include "pictor/animation/animation_types.h"

#include <cstdio>
#include <string>
#include <vector>

namespace pictor_fbx_viewer {

/// Writes {"bones":[{"name","parent","head":[x,y,z]}...],"blendshapes":[...]}.
/// Returns false when the stream reports a write error.
bool write_channel_list_json(std::FILE* out, const pictor::SkeletonDescriptor& skeleton,
                             const std::vector<std::string>& shape_names);

} // namespace pictor_fbx_viewer
