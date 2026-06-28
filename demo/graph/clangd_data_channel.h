#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "data_channel.h"

namespace pictor::graph {

/// ── clangd call-hierarchy contract (mirrors Iter's `lsp.ts`) ──
/// These structs are the on-the-wire shape Iter already produces from clangd's
/// `callHierarchy/{incomingCalls,outgoingCalls}` + `textDocument/references`.
/// They are plain data so the host can fill them from JSON without Pictor ever
/// depending on Tauri / serde.

struct ClangdPosition { int line = 0; int character = 0; };
struct ClangdRange    { ClangdPosition start; ClangdPosition end; };

struct ClangdItem {
    std::string name;
    int         kind = 0;     // LSP SymbolKind
    std::string detail;
    std::string uri;          // file:// URI
    ClangdRange range;
    ClangdRange selectionRange;
};

struct ClangdIncomingCall { ClangdItem from; };  // caller -> origin
struct ClangdOutgoingCall { ClangdItem to;   };  // origin -> callee
struct ClangdReference    { std::string uri; ClangdRange range; };

/// One node's neighbourhood as returned by a single fetch.
struct ClangdRelation {
    ClangdItem                     origin;
    std::vector<ClangdIncomingCall> incoming;
    std::vector<ClangdOutgoingCall> outgoing;
    std::vector<ClangdReference>    references;
};

/// Stable node id from a symbol location (uri + definition line + name). Equal
/// symbols across fetches hash equal, so duplicate callers/callees collapse.
uint64_t clangd_node_id(const std::string& uri, int line, const std::string& name);

/// LSP SymbolKind -> graph NodeKind (color / synthetic-snippet shape).
NodeKind clangd_kind_to_node_kind(int symbol_kind);

/// Pure mapping of one clangd neighbourhood to the `{nodes, edges}` contract.
/// Edge direction follows call semantics: caller -> origin, origin -> callee,
/// origin -> reference.
GraphData clangd_relation_to_graph_data(const ClangdRelation& rel);

/// IDataChannel backed by clangd over an injected fetch. The fetch is the
/// Rust(Iter) <-> C++(Pictor) IPC boundary: given a node id it returns that
/// node's clangd neighbourhood (the host implements it by invoking the Tauri
/// command and parsing the JSON envelope). Pictor never sees the transport.
class ClangdDataChannel : public IDataChannel {
public:
    using Fetch = std::function<ClangdRelation(uint64_t node_id)>;

    ClangdDataChannel(uint64_t root_id, Fetch fetch)
        : root_id_(root_id), fetch_(std::move(fetch)) {}

    GraphData initial() override;
    GraphData request_children(uint64_t node_id) override;

private:
    uint64_t root_id_;
    Fetch    fetch_;
};

} // namespace pictor::graph
