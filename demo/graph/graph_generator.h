#pragma once

#include <cstdint>

#include "graph_store.h"

namespace pictor::graph {

/// Generate a deterministic synthetic graph (no real clangd data yet). The
/// topology is layered — each node connects forward to a few nodes in the next
/// layer, approximating a call-hierarchy fan-out.
///
/// `scatter` only affects the *initial* node positions: when false they are
/// placed in tidy layers; when true they start randomly so the Sugiyama layout
/// (Step 3) visibly snaps them into place. The topology is identical either way.
GraphStore generate_layered_graph(uint32_t node_count, uint32_t seed = 1u,
                                  bool scatter = false);

} // namespace pictor::graph
