#pragma once
#include "../fbx_viewer/packed_mesh.h"
#include "pn_patch.h"
#include <cstddef>

namespace pictor_kuzuha {
struct RefinementStats {
    size_t source_triangles=0, output_triangles=0, protected_edges=0;
    double rms_displacement=0;
    float max_displacement=0;
};
class PnModel {
public:
    explicit PnModel(pictor_fbx_viewer::PackedMesh source);
    pictor_fbx_viewer::PackedMesh sample(unsigned factor, float strength, RefinementStats& stats) const;
    const pictor_fbx_viewer::PackedMesh& source() const { return source_; }
    /// @implements SPEC-PC-KUZUHA-RAYMARCH
    const std::vector<PnPatch>& patches() const { return patches_; }
private:
    pictor_fbx_viewer::PackedMesh source_;
    std::vector<PnPatch> patches_;
    size_t protected_edges_=0;
};
}
