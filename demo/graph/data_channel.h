#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "graph_store.h"

namespace pictor::graph {

/// One node in the relation-graph contract. `id` is the stable identity used to
/// dedup across fetches (a clangd symbol's USR/location hash on the real path).
struct GraphNodeDesc {
    uint64_t    id;
    std::string name;
    NodeKind    kind = NodeKind::Other;
};

/// One directed relation. Endpoints reference node ids (not handles) so the
/// payload is position- and order-independent across the IPC boundary.
struct GraphEdgeDesc {
    uint64_t from_id;
    uint64_t to_id;
    EdgeKind kind = EdgeKind::Call;
};

/// The `{nodes, edges}` contract exchanged with the data source. Equivalent to
/// Iter's React-Flow `{ nodes, edges }`, but flattened to plain ids so both the
/// synthetic generator and the clangd bridge speak the same shape.
struct GraphData {
    std::vector<GraphNodeDesc> nodes;
    std::vector<GraphEdgeDesc> edges;
};

/// Source of relation-graph data for the Tier B widget. Decouples the view from
/// where the contract is produced: a deterministic generator today, the clangd
/// call-hierarchy bridge (Iter, over IPC) tomorrow. Keeping this an interface is
/// the Rust(Iter) <-> C++(Pictor) boundary — only the `{nodes, edges}` payload
/// crosses it, never Vulkan or Tauri types (Pictor stays the lowest layer).
class IDataChannel {
public:
    virtual ~IDataChannel() = default;

    /// Roots plus their first level — the graph shown before any expand.
    virtual GraphData initial() = 0;

    /// Callees / callers / references of one node, addressed by its stable id.
    /// Returned nodes already present (by id) are reused by the caller; only the
    /// genuinely new ones are appended (incremental expand).
    virtual GraphData request_children(uint64_t node_id) = 0;
};

} // namespace pictor::graph
