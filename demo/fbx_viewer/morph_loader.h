// Builds MorphTargets for the packed viewer mesh from the importer's
// SkinMeshDescriptor::morph_target_names / morph_deltas
// (SPEC-PC-FBX-TRACK-PLAYBACK).
#pragma once

#include "morph_targets.h"
#include "packed_mesh.h"

#include "pictor/animation/fbx_importer.h"

namespace pictor_fbx_viewer {

/// Map each importer skin mesh onto its vertex range in `mesh` and collect
/// sparse shape deltas. Both the importer and pack_mesh_from_fbx() walk
/// the scene geometries in the same order; geometries the packer skips
/// (no triangles) are skipped here as well. Returns an empty set and
/// prints a warning when the two layouts disagree. `names` of the result is
/// the model's shape list (first-seen order) used by the track parser.
MorphTargets build_morph_targets(const pictor::FBXImportResult& result, const PackedMesh& mesh);

} // namespace pictor_fbx_viewer
