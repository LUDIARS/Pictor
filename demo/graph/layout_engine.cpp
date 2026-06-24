#include "layout_engine.h"

#include <algorithm>
#include <numeric>

namespace pictor::graph {

namespace {

// CSR adjacency built from an edge subset.
struct Csr {
    std::vector<uint32_t> start;  // size n+1
    std::vector<uint32_t> items;  // size edge_count
};

Csr build_csr(uint32_t n, const std::vector<uint32_t>& src, const std::vector<uint32_t>& dst) {
    Csr c;
    c.start.assign(n + 1, 0);
    for (uint32_t v : src) ++c.start[v + 1];
    for (uint32_t i = 0; i < n; ++i) c.start[i + 1] += c.start[i];
    c.items.resize(src.size());
    std::vector<uint32_t> cur(c.start.begin(), c.start.end() - 1);
    for (size_t e = 0; e < src.size(); ++e)
        c.items[cur[src[e]]++] = dst[e];
    return c;
}

} // namespace

LayoutResult compute_layered_layout(uint32_t n,
                                    const std::vector<uint32_t>& from,
                                    const std::vector<uint32_t>& to,
                                    const LayoutParams& p) {
    LayoutResult result;
    if (n == 0) { result.ok = true; return result; }
    result.x.assign(n, 0.0f);
    result.y.assign(n, 0.0f);

    // ── 1. Cycle removal: iterative DFS that keeps every edge except those
    // pointing at a node currently on the DFS stack (back edges). The acyclic
    // edge subset (af/at) is collected directly during traversal so its order
    // is self-consistent (no separate index mapping).
    const Csr out_all = build_csr(n, from, to);
    std::vector<uint8_t> color(n, 0);      // 0=white, 1=on-stack(gray), 2=done(black)
    std::vector<uint32_t> af, at;
    af.reserve(from.size());
    at.reserve(to.size());

    struct Frame { uint32_t node; uint32_t cursor; };
    std::vector<Frame> stack;
    stack.reserve(64);
    for (uint32_t s = 0; s < n; ++s) {
        if (color[s] != 0) continue;
        color[s] = 1;
        stack.push_back({s, out_all.start[s]});
        while (!stack.empty()) {
            Frame& f = stack.back();
            if (f.cursor < out_all.start[f.node + 1]) {
                const uint32_t u = f.node;
                const uint32_t v = out_all.items[f.cursor];
                ++f.cursor;                      // advance before any push (f may dangle after)
                if (color[v] == 1) {
                    // back edge -> drop
                } else {
                    af.push_back(u);             // tree / forward / cross edge -> keep
                    at.push_back(v);
                    if (color[v] == 0) {
                        color[v] = 1;
                        stack.push_back({v, out_all.start[v]});
                    }
                }
            } else {
                color[f.node] = 2;
                stack.pop_back();
            }
        }
    }

    // ── 2. Layer assignment via longest path (Kahn topo order on the DAG).
    const Csr out_acyc = build_csr(n, af, at);
    std::vector<uint32_t> indeg(n, 0);
    for (uint32_t v : at) ++indeg[v];

    std::vector<uint32_t> order;            // topological order
    order.reserve(n);
    std::vector<uint32_t> queue;
    queue.reserve(n);
    for (uint32_t v = 0; v < n; ++v)
        if (indeg[v] == 0) queue.push_back(v);
    std::vector<uint32_t> indeg_work = indeg;
    for (size_t qi = 0; qi < queue.size(); ++qi) {
        const uint32_t u = queue[qi];
        order.push_back(u);
        for (uint32_t k = out_acyc.start[u]; k < out_acyc.start[u + 1]; ++k) {
            const uint32_t v = out_acyc.items[k];
            if (--indeg_work[v] == 0) queue.push_back(v);
        }
    }
    // Any nodes left out (shouldn't happen after cycle removal) get appended.
    if (order.size() < n) {
        std::vector<uint8_t> seen(n, 0);
        for (uint32_t v : order) seen[v] = 1;
        for (uint32_t v = 0; v < n; ++v) if (!seen[v]) order.push_back(v);
    }

    std::vector<int> layer(n, 0);
    int max_layer = 0;
    for (uint32_t u : order) {
        for (uint32_t k = out_acyc.start[u]; k < out_acyc.start[u + 1]; ++k) {
            const uint32_t v = out_acyc.items[k];
            if (layer[u] + 1 > layer[v]) layer[v] = layer[u] + 1;
            if (layer[v] > max_layer) max_layer = layer[v];
        }
    }

    // ── 3. Group into layers; barycenter crossing reduction.
    std::vector<std::vector<uint32_t>> layers(max_layer + 1);
    for (uint32_t v = 0; v < n; ++v) layers[layer[v]].push_back(v);

    std::vector<uint32_t> pos(n, 0);        // order index within layer
    auto reindex = [&](const std::vector<uint32_t>& lyr) {
        for (uint32_t i = 0; i < lyr.size(); ++i) pos[lyr[i]] = i;
    };
    for (auto& lyr : layers) reindex(lyr);

    // Neighbour CSR using acyclic edges, both directions.
    const Csr in_acyc = build_csr(n, at, af);  // reversed

    auto barycenter = [&](uint32_t node, bool use_predecessors) -> float {
        const Csr& adj = use_predecessors ? in_acyc : out_acyc;
        const uint32_t b = adj.start[node];
        const uint32_t e = adj.start[node + 1];
        if (e == b) return static_cast<float>(pos[node]); // keep place if isolated
        float sum = 0.0f;
        for (uint32_t k = b; k < e; ++k) sum += static_cast<float>(pos[adj.items[k]]);
        return sum / static_cast<float>(e - b);
    };

    for (int s = 0; s < p.sweeps; ++s) {
        const bool downward = (s % 2 == 0); // even: order by predecessors (top-down)
        if (downward) {
            for (int L = 1; L <= max_layer; ++L) {
                auto& lyr = layers[L];
                std::stable_sort(lyr.begin(), lyr.end(), [&](uint32_t a, uint32_t b) {
                    return barycenter(a, true) < barycenter(b, true);
                });
                reindex(lyr);
            }
        } else {
            for (int L = max_layer - 1; L >= 0; --L) {
                auto& lyr = layers[L];
                std::stable_sort(lyr.begin(), lyr.end(), [&](uint32_t a, uint32_t b) {
                    return barycenter(a, false) < barycenter(b, false);
                });
                reindex(lyr);
            }
        }
    }

    // ── 4. Coordinate assignment (centered per layer).
    for (int L = 0; L <= max_layer; ++L) {
        const auto& lyr = layers[L];
        const float count = static_cast<float>(lyr.size());
        const float x0 = -(count - 1.0f) * 0.5f * p.node_gap;
        for (uint32_t i = 0; i < lyr.size(); ++i) {
            const uint32_t v = lyr[i];
            result.x[v] = x0 + static_cast<float>(i) * p.node_gap;
            result.y[v] = static_cast<float>(L) * p.layer_gap;
        }
    }

    result.ok = true;
    return result;
}

} // namespace pictor::graph
