#pragma once

#include <cstdint>
#include <functional>
#include <list>
#include <string>
#include <unordered_map>
#include <utility>

namespace pictor::graph {

/// Bounded LRU cache of code snippets keyed by node id.
///
/// Models the NEAR-LOD snippet path: snippets are fetched lazily (only when a
/// node is on screen and large enough) and a fixed number are kept resident —
/// the rest are evicted, so memory stays bounded regardless of graph size. The
/// miss callback stands in for the (later async) clangd `read_snippet`.
class SnippetCache {
public:
    explicit SnippetCache(size_t capacity = 256) : capacity_(capacity ? capacity : 1) {}

    /// Return the snippet for `key`, computing it via `miss` on a cache miss.
    /// The reference is valid until the next get() call (callers consume it
    /// immediately). A hit promotes the entry to most-recently-used.
    const std::string& get(uint64_t key, const std::function<std::string()>& miss);

    size_t size()     const { return map_.size(); }
    size_t capacity() const { return capacity_; }

private:
    using Entry = std::pair<uint64_t, std::string>;

    size_t capacity_;
    std::list<Entry> lru_;   // front = most-recently-used
    std::unordered_map<uint64_t, std::list<Entry>::iterator> map_;
};

} // namespace pictor::graph
