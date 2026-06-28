#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace pictor::graph {

using NodeHandle = uint32_t;
constexpr NodeHandle INVALID_NODE = 0xFFFFFFFFu;

/// Sentinel passed to add_node to request an auto-assigned stable id (== handle).
/// A caller wiring real data (clangd symbols, Step 6 expand) passes its own id so
/// duplicate children collapse onto the existing node via id_to_index().
constexpr uint64_t AUTO_NODE_ID = 0xFFFFFFFFFFFFFFFFull;

/// Semantic kind of a graph node (drives default color). Mirrors the
/// caller/callee/reference vocabulary Iter surfaces from clangd.
enum class NodeKind : uint8_t {
    Function = 0,
    Method,
    Variable,
    Type,
    Other,
    Count
};

/// Edge relation kind. Phase 1 only renders them as flat lines, but the kind
/// is carried so later phases can color/route by relation.
enum class EdgeKind : uint8_t {
    Call = 0,
    Reference,
    Override,
    Count
};

/// Struct-of-Arrays graph store (Iter relation-graph, Step 1).
///
/// DoD discipline (Pictor CLAUDE.md): the per-node parallel arrays are the only
/// data the render / cull hot path touches, indexed by NodeHandle. Symbol
/// strings and the id→handle map live on the cold side and are read once at
/// build / expand time, never per frame. Node arrays are append-only so live
/// handles stay stable (required for animation + incremental expand later).
class GraphStore {
public:
    struct Nodes {
        std::vector<float>    cx, cy;   // center, world units
        std::vector<float>    w, h;     // box size, world units
        std::vector<uint32_t> rgba;     // packed 0xRRGGBBAA
        std::vector<uint8_t>  kind;     // NodeKind
        std::vector<uint8_t>  flags;    // selected/hovered/... (Step 1: unused)
        std::vector<std::string> label; // symbol name (cold; only NEAR-LOD labels read it)
        std::vector<uint64_t> id;       // stable external id (cold; expand/dedup only)
    };

    struct Edges {
        std::vector<uint32_t> from;     // NodeHandle
        std::vector<uint32_t> to;       // NodeHandle
        std::vector<uint8_t>  kind;     // EdgeKind
    };

    void reserve(size_t node_capacity, size_t edge_capacity);

    NodeHandle add_node(float cx, float cy, float w, float h,
                        uint32_t rgba, NodeKind kind, std::string label = {},
                        uint64_t node_id = AUTO_NODE_ID);
    void       add_edge(NodeHandle from, NodeHandle to, EdgeKind kind = EdgeKind::Call);

    /// Handle previously registered for `node_id`, or INVALID_NODE if unseen.
    /// Lets incremental expand collapse duplicate children onto existing nodes.
    NodeHandle find(uint64_t node_id) const;

    /// Stable external id of a node (the value passed to add_node).
    uint64_t   id_of(NodeHandle handle) const { return nodes_.id[handle]; }

    /// Overwrite all node centers (e.g. with a freshly computed layout). Sizes
    /// must match node_count(); mismatched input is ignored.
    void set_positions(const std::vector<float>& xs, const std::vector<float>& ys);

    size_t       node_count() const { return nodes_.cx.size(); }
    size_t       edge_count() const { return edges_.from.size(); }
    const Nodes& nodes()      const { return nodes_; }
    const Edges& edges()      const { return edges_; }

private:
    Nodes nodes_;
    Edges edges_;
    std::unordered_map<uint64_t, NodeHandle> id_to_index_;  // cold; build/expand only
};

} // namespace pictor::graph
