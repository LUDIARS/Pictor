#pragma once

#include <cstdint>
#include <vector>

namespace pictor::graph {

class GraphStore;

/// Uniform spatial grid over the graph's node boxes (built once; positions are
/// static in Step 2). Backs the two scale-critical operations:
///   - viewport cull: query_visible() returns exactly the nodes whose box meets
///     the view rect, so draw work is O(visible) not O(total).
///   - hit-test: pick() maps a world point to the topmost node under it.
///
/// Storage is CSR (counts -> prefix-sum offsets -> packed handle array), and the
/// grid keeps its own flat copy of node box extents so queries are self-contained
/// and cache-local (DoD: no pointer-chasing into the store on the hot path).
class SpatialGrid {
public:
    void build(const GraphStore& store);

    /// Append handles of nodes whose box intersects [min,max] (world) to `out`.
    /// `out` is cleared first. Result is exact (box-vs-rect refined).
    void query_visible(float min_x, float min_y, float max_x, float max_y,
                       std::vector<uint32_t>& out) const;

    /// Topmost (largest handle) node whose box contains the world point, or
    /// 0xFFFFFFFF if none.
    uint32_t pick(float world_x, float world_y) const;

    size_t node_count() const { return cx_.size(); }

private:
    int cell_index(float x, float y) const;

    // Flat copies of node box centers / sizes (index == NodeHandle).
    std::vector<float> cx_, cy_, hw_, hh_;  // half-width / half-height

    // CSR buckets.
    std::vector<uint32_t> cell_start_;  // size = cells + 1
    std::vector<uint32_t> items_;       // size = node count

    float  origin_x_ = 0.0f, origin_y_ = 0.0f;
    float  cell_size_ = 256.0f;
    int    nx_ = 1, ny_ = 1;
    float  max_hw_ = 0.0f, max_hh_ = 0.0f;  // padding for boundary boxes
};

} // namespace pictor::graph
