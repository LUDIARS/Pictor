#include "synthetic_data_channel.h"

#include <string>

#include "graph_generator.h"
#include "graph_store.h"

namespace pictor::graph {

namespace {

constexpr const char* kKindPrefix[static_cast<int>(NodeKind::Count)] = {
    "fn_", "m_", "var_", "Type_", "sym_"
};

// SplitMix64: cheap, well-distributed 64-bit hash for deterministic id/kind
// derivation without any shared state.
uint64_t mix64(uint64_t x) {
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

// Children live in the high id namespace so they never collide with the initial
// level's auto-ids (0 .. node_count).
constexpr uint64_t kExpandBit = 1ull << 63;

} // namespace

GraphData SyntheticDataChannel::initial() {
    GraphData out;
    const GraphStore store = generate_layered_graph(node_count_, seed_, false);

    const auto& n = store.nodes();
    out.nodes.reserve(store.node_count());
    for (size_t i = 0; i < store.node_count(); ++i)
        out.nodes.push_back({store.id_of(static_cast<NodeHandle>(i)), n.label[i],
                             static_cast<NodeKind>(n.kind[i])});

    const auto& e = store.edges();
    out.edges.reserve(store.edge_count());
    for (size_t k = 0; k < store.edge_count(); ++k)
        out.edges.push_back({store.id_of(e.from[k]), store.id_of(e.to[k]),
                             static_cast<EdgeKind>(e.kind[k])});
    return out;
}

GraphData SyntheticDataChannel::request_children(uint64_t node_id) {
    GraphData out;
    const uint64_t h0 = mix64(node_id ^ (seed_ + 1u));
    const uint32_t fanout = 1u + static_cast<uint32_t>(h0 % 3u);  // 1..3 children

    for (uint32_t f = 0; f < fanout; ++f) {
        const uint64_t h = mix64(h0 + f);
        const uint64_t child_id = (h >> 1) | kExpandBit;
        const auto kind = static_cast<NodeKind>(h % static_cast<uint64_t>(NodeKind::Count));
        std::string name = std::string(kKindPrefix[static_cast<int>(kind)]) +
                           std::to_string(child_id % 100000ull);
        out.nodes.push_back({child_id, std::move(name), kind});
        out.edges.push_back({node_id, child_id, EdgeKind::Call});
    }
    return out;
}

} // namespace pictor::graph
