#include "graph_store.h"

#include <utility>

namespace pictor::graph {

void GraphStore::reserve(size_t node_capacity, size_t edge_capacity) {
    nodes_.cx.reserve(node_capacity);
    nodes_.cy.reserve(node_capacity);
    nodes_.w.reserve(node_capacity);
    nodes_.h.reserve(node_capacity);
    nodes_.rgba.reserve(node_capacity);
    nodes_.kind.reserve(node_capacity);
    nodes_.flags.reserve(node_capacity);
    nodes_.label.reserve(node_capacity);
    nodes_.id.reserve(node_capacity);
    id_to_index_.reserve(node_capacity);

    edges_.from.reserve(edge_capacity);
    edges_.to.reserve(edge_capacity);
    edges_.kind.reserve(edge_capacity);
}

NodeHandle GraphStore::add_node(float cx, float cy, float w, float h,
                                uint32_t rgba, NodeKind kind, std::string label,
                                uint64_t node_id) {
    const NodeHandle handle = static_cast<NodeHandle>(nodes_.cx.size());
    if (node_id == AUTO_NODE_ID) node_id = handle;
    nodes_.cx.push_back(cx);
    nodes_.cy.push_back(cy);
    nodes_.w.push_back(w);
    nodes_.h.push_back(h);
    nodes_.rgba.push_back(rgba);
    nodes_.kind.push_back(static_cast<uint8_t>(kind));
    nodes_.flags.push_back(0);
    nodes_.label.push_back(std::move(label));
    nodes_.id.push_back(node_id);
    id_to_index_.emplace(node_id, handle);
    return handle;
}

NodeHandle GraphStore::find(uint64_t node_id) const {
    const auto it = id_to_index_.find(node_id);
    return (it == id_to_index_.end()) ? INVALID_NODE : it->second;
}

void GraphStore::set_positions(const std::vector<float>& xs, const std::vector<float>& ys) {
    if (xs.size() != nodes_.cx.size() || ys.size() != nodes_.cy.size()) return;
    nodes_.cx = xs;
    nodes_.cy = ys;
}

void GraphStore::add_edge(NodeHandle from, NodeHandle to, EdgeKind kind) {
    edges_.from.push_back(from);
    edges_.to.push_back(to);
    edges_.kind.push_back(static_cast<uint8_t>(kind));
}

} // namespace pictor::graph
