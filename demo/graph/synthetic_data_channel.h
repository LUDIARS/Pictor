#pragma once

#include <cstdint>

#include "data_channel.h"

namespace pictor::graph {

/// Deterministic stand-in for the clangd bridge: a synthetic call graph whose
/// initial level mirrors the demo generator and whose children are derived
/// reproducibly from a parent id (so re-expanding a node is idempotent). Replace
/// with ClangdDataChannel once Iter's IPC is wired.
class SyntheticDataChannel : public IDataChannel {
public:
    SyntheticDataChannel(uint32_t node_count, uint32_t seed)
        : node_count_(node_count ? node_count : 1), seed_(seed) {}

    GraphData initial() override;
    GraphData request_children(uint64_t node_id) override;

private:
    uint32_t node_count_;
    uint32_t seed_;
};

} // namespace pictor::graph
