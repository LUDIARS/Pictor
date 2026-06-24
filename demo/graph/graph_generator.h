#pragma once

#include <cstdint>

#include "graph_store.h"

namespace pictor::graph {

/// Generate a deterministic, layered synthetic graph for the Step 1 stress
/// test (no real clangd data yet). Positions are fixed at generation time
/// (Sugiyama layout is a later step); the goal is only to prove that N boxes +
/// edges pan/zoom without melting.
///
/// Nodes are laid out in horizontal layers; each node connects forward to a
/// few nodes in the next layer, approximating a call hierarchy fan-out.
GraphStore generate_layered_graph(uint32_t node_count, uint32_t seed = 1u);

} // namespace pictor::graph
