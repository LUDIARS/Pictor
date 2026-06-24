#include "snippet_cache.h"

namespace pictor::graph {

const std::string& SnippetCache::get(uint64_t key,
                                     const std::function<std::string()>& miss) {
    auto it = map_.find(key);
    if (it != map_.end()) {
        // Hit: promote to MRU.
        lru_.splice(lru_.begin(), lru_, it->second);
        return it->second->second;
    }

    // Miss: fetch, insert at MRU, evict LRU if over capacity.
    lru_.emplace_front(key, miss());
    map_[key] = lru_.begin();
    if (map_.size() > capacity_) {
        const Entry& victim = lru_.back();
        map_.erase(victim.first);
        lru_.pop_back();
    }
    return lru_.front().second;
}

} // namespace pictor::graph
