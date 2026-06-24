#pragma once

#include <cstdint>
#include <vector>

namespace pictor::graph {

struct LayoutParams {
    float layer_gap = 140.0f;  // vertical distance between layers (world units)
    float node_gap  = 190.0f;  // horizontal distance between nodes in a layer
    int   sweeps    = 4;       // barycenter crossing-reduction passes
};

struct LayoutResult {
    std::vector<float> x;
    std::vector<float> y;
    bool ok = false;
};

/// Sugiyama-style layered layout for a directed graph (Iter call hierarchy).
/// Pure / thread-safe (no shared state) so it can run on a worker thread:
///   1. remove cycles (DFS back-edge detection)
///   2. assign layers by longest path on the acyclic DAG
///   3. reduce edge crossings with barycenter sweeps
///   4. assign coordinates (layer -> y, order-in-layer -> x, centered)
///
/// `from` / `to` are parallel edge arrays of node handles in [0, node_count).
LayoutResult compute_layered_layout(uint32_t node_count,
                                    const std::vector<uint32_t>& from,
                                    const std::vector<uint32_t>& to,
                                    const LayoutParams& params = {});

} // namespace pictor::graph
