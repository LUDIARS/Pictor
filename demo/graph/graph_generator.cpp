#include "graph_generator.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace pictor::graph {

namespace {

/// Small deterministic LCG so the demo graph is reproducible across runs
/// without pulling in <random> (and without any global state).
struct Lcg {
    uint32_t state;
    explicit Lcg(uint32_t seed) : state(seed ? seed : 0x9E3779B9u) {}
    uint32_t next() {
        state = state * 1664525u + 1013904223u;
        return state;
    }
    // Uniform in [0, n).
    uint32_t below(uint32_t n) { return n ? (next() % n) : 0u; }
    float unit() { return static_cast<float>(next() >> 8) / 16777216.0f; }
};

// Palette indexed by NodeKind — kept readable on the dark clear color.
constexpr uint32_t kKindColor[static_cast<int>(NodeKind::Count)] = {
    0x4FA3FFFFu, // Function  — blue
    0x6BD389FFu, // Method    — green
    0xE0B04AFFu, // Variable  — amber
    0xC080F0FFu, // Type      — violet
    0x9AA0A6FFu, // Other     — grey
};

} // namespace

GraphStore generate_layered_graph(uint32_t node_count, uint32_t seed, bool scatter) {
    GraphStore store;
    if (node_count == 0) return store;

    Lcg rng(seed);

    // Roughly square aspect: ~sqrt(N) nodes per layer.
    const uint32_t per_layer =
        std::max(1u, static_cast<uint32_t>(std::sqrt(static_cast<double>(node_count))));
    const uint32_t layer_count = (node_count + per_layer - 1u) / per_layer;
    // Scatter box half-extent (used only for initial positions when scatter=true).
    const float scatter_r = static_cast<float>(per_layer) * 100.0f;

    constexpr float kNodeW    = 120.0f;
    constexpr float kNodeH    = 44.0f;
    constexpr float kGapX     = 70.0f;
    constexpr float kGapY     = 90.0f;
    const float     stride_x  = kNodeW + kGapX;
    const float     stride_y  = kNodeH + kGapY;

    store.reserve(node_count, static_cast<size_t>(node_count) * 2u);

    // Remember where each layer's nodes start so edges can target the next one.
    std::vector<uint32_t> layer_begin;
    layer_begin.reserve(layer_count + 1u);

    uint32_t made = 0;
    for (uint32_t layer = 0; layer < layer_count && made < node_count; ++layer) {
        layer_begin.push_back(made);
        const uint32_t in_this = std::min(per_layer, node_count - made);

        // Center each layer horizontally for a tidy spread.
        const float row_w = static_cast<float>(in_this) * stride_x;
        const float x0    = -row_w * 0.5f;
        const float y     = static_cast<float>(layer) * stride_y;

        for (uint32_t i = 0; i < in_this; ++i) {
            const auto kind = static_cast<NodeKind>(rng.below(static_cast<uint32_t>(NodeKind::Count)));
            float px, py;
            if (scatter) {
                px = (rng.unit() - 0.5f) * 2.0f * scatter_r;
                py = (rng.unit() - 0.5f) * 2.0f * scatter_r;
            } else {
                // Slight jitter so the grid does not look mechanical.
                px = x0 + static_cast<float>(i) * stride_x + (rng.unit() - 0.5f) * kGapX * 0.5f;
                py = y;
            }
            store.add_node(px, py, kNodeW, kNodeH,
                           kKindColor[static_cast<int>(kind)], kind);
            ++made;
        }
    }
    layer_begin.push_back(made);

    // Forward edges: each node points at 1–3 nodes in the next layer.
    for (uint32_t layer = 0; layer + 1u < static_cast<uint32_t>(layer_begin.size()) - 1u; ++layer) {
        const uint32_t begin = layer_begin[layer];
        const uint32_t end   = layer_begin[layer + 1u];
        const uint32_t next_begin = layer_begin[layer + 1u];
        const uint32_t next_end   = layer_begin[layer + 2u];
        const uint32_t next_size  = next_end - next_begin;
        if (next_size == 0) continue;

        for (uint32_t n = begin; n < end; ++n) {
            const uint32_t fanout = 1u + rng.below(3u);
            for (uint32_t f = 0; f < fanout; ++f) {
                const uint32_t target = next_begin + rng.below(next_size);
                store.add_edge(n, target, EdgeKind::Call);
            }
        }
    }

    return store;
}

} // namespace pictor::graph
