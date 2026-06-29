#include "clangd_data_channel.h"

namespace pictor::graph {

namespace {

// FNV-1a over the symbol's identifying triple; cheap and stable across runs.
uint64_t fnv1a(const std::string& s, uint64_t h) {
    for (const char c : s) {
        h ^= static_cast<uint8_t>(c);
        h *= 0x100000001B3ull;
    }
    return h;
}

uint64_t id_for(const ClangdItem& item) {
    return clangd_node_id(item.uri, item.selectionRange.start.line, item.name);
}

} // namespace

uint64_t clangd_node_id(const std::string& uri, int line, const std::string& name) {
    uint64_t h = 0xCBF29CE484222325ull;
    h = fnv1a(uri, h);
    h ^= static_cast<uint64_t>(static_cast<uint32_t>(line));
    h *= 0x100000001B3ull;
    h = fnv1a(name, h);
    return h;
}

NodeKind clangd_kind_to_node_kind(int symbol_kind) {
    switch (symbol_kind) {
    case 12: return NodeKind::Function;            // Function
    case 6:  return NodeKind::Method;              // Method
    case 9:  return NodeKind::Method;              // Constructor
    case 8:  return NodeKind::Variable;            // Field
    case 13: return NodeKind::Variable;            // Variable
    case 5:  return NodeKind::Type;                // Class
    case 23: return NodeKind::Type;                // Struct
    case 10: return NodeKind::Type;                // Enum
    case 11: return NodeKind::Type;                // Interface
    default: return NodeKind::Other;
    }
}

GraphData clangd_relation_to_graph_data(const ClangdRelation& rel) {
    GraphData out;

    const uint64_t origin_id = id_for(rel.origin);
    out.nodes.push_back({origin_id, rel.origin.name,
                         clangd_kind_to_node_kind(rel.origin.kind)});

    for (const auto& c : rel.incoming) {
        const uint64_t id = id_for(c.from);
        out.nodes.push_back({id, c.from.name, clangd_kind_to_node_kind(c.from.kind)});
        out.edges.push_back({id, origin_id, EdgeKind::Call});       // caller -> origin
    }
    for (const auto& c : rel.outgoing) {
        const uint64_t id = id_for(c.to);
        out.nodes.push_back({id, c.to.name, clangd_kind_to_node_kind(c.to.kind)});
        out.edges.push_back({origin_id, id, EdgeKind::Call});       // origin -> callee
    }
    for (const auto& r : rel.references) {
        const uint64_t id = clangd_node_id(r.uri, r.range.start.line, r.uri);
        out.nodes.push_back({id, std::string{}, NodeKind::Other});
        out.edges.push_back({origin_id, id, EdgeKind::Reference});  // origin -> ref
    }
    return out;
}

GraphData ClangdDataChannel::initial() {
    if (!fetch_) return {};
    return clangd_relation_to_graph_data(fetch_(root_id_));
}

GraphData ClangdDataChannel::request_children(uint64_t node_id) {
    if (!fetch_) return {};
    return clangd_relation_to_graph_data(fetch_(node_id));
}

} // namespace pictor::graph
