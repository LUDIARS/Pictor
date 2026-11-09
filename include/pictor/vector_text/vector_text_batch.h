#pragma once

#include <cstdint>
#include <vector>

namespace pictor {

/// One glyph to draw. `m` is the 2x2 map from em units (y-up) to screen pixels
/// (column-major: (m[0], m[1]) is where the em x axis goes); `tx, ty` is where
/// the glyph origin lands. Scale and rotation are free; the feather stays
/// `feather_px` wide under similarity transforms.
struct alignas(16) VectorTextInstance {
    float m[4]       = {1, 0, 0, -1};
    float tx         = 0.0f;
    float ty         = 0.0f;
    float feather_px = 1.0f;
    float pad        = 0.0f;
    float color[4]   = {1, 1, 1, 1};
};
static_assert(sizeof(VectorTextInstance) == 48, "VectorTextInstance is uploaded as-is");

/// Consecutive instances that share one mesh: one instanced draw.
struct VectorTextDraw {
    uint32_t mesh           = 0;
    uint32_t first_instance = 0;
    uint32_t instance_count = 0;
};

/// The glyphs of one frame. Instances and their mesh ids are parallel flat arrays.
class VectorTextBatch {
public:
    void clear();
    void add(uint32_t mesh, const VectorTextInstance& instance);

    /// Groups instances by mesh and fills `draws()`. The sort is stable, so
    /// copies of one glyph keep their order. Reorders `instances()`.
    void build_draws();

    [[nodiscard]] const std::vector<VectorTextInstance>& instances() const { return instances_; }
    [[nodiscard]] const std::vector<uint32_t>& meshes() const { return meshes_; }
    [[nodiscard]] const std::vector<VectorTextDraw>& draws() const { return draws_; }
    [[nodiscard]] bool empty() const { return instances_.empty(); }

private:
    std::vector<VectorTextInstance> instances_;
    std::vector<uint32_t>           meshes_;
    std::vector<VectorTextDraw>     draws_;
    // Scratch kept between frames so build_draws() does not allocate once warm.
    std::vector<uint32_t>           order_;
    std::vector<VectorTextInstance> sorted_instances_;
    std::vector<uint32_t>           sorted_meshes_;
};

} // namespace pictor
