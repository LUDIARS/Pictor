#include "pictor/vector_text/glyph_mesh_cache.h"

#include "pictor/vector_text/glyph_mesh_builder.h"
#include "pictor/vector_text/size_tier.h"

namespace pictor {

uint64_t GlyphMeshCache::key_of(FontHandle font, uint32_t codepoint, uint32_t tier) {
    // Codepoints need 21 bits and tiers 4, so both fit under the font handle.
    return (static_cast<uint64_t>(font) << 32) | (static_cast<uint64_t>(tier & 0xFFU) << 24) |
           (codepoint & 0xFFFFFFU);
}

namespace {
size_t slot_hash(uint64_t key, size_t mask) {
    key ^= key >> 33;
    key *= 0xff51afd7ed558ccdULL;
    key ^= key >> 33;
    return static_cast<size_t>(key) & mask;
}
}  // namespace

uint32_t GlyphMeshCache::find(uint64_t key) const {
    if (slots_.empty()) return kEmpty;
    const size_t mask = slots_.size() - 1;
    for (size_t i = slot_hash(key, mask);; i = (i + 1) & mask) {
        const Slot& slot = slots_[i];
        if (slot.mesh == kEmpty) return kEmpty;
        if (slot.key == key) return slot.mesh;
    }
}

void GlyphMeshCache::insert(uint64_t key, uint32_t mesh) {
    // Keep the load factor under one half so probes stay short.
    if ((ranges_.size() + 1) * 2 > slots_.size()) grow_table();
    const size_t mask = slots_.size() - 1;
    size_t i = slot_hash(key, mask);
    while (slots_[i].mesh != kEmpty) i = (i + 1) & mask;
    slots_[i] = Slot{key, mesh};
}

void GlyphMeshCache::grow_table() {
    std::vector<Slot> old;
    old.swap(slots_);
    slots_.assign(old.empty() ? 64 : old.size() * 2, Slot{});
    const size_t mask = slots_.size() - 1;
    for (const Slot& slot : old) {
        if (slot.mesh == kEmpty) continue;
        size_t i = slot_hash(slot.key, mask);
        while (slots_[i].mesh != kEmpty) i = (i + 1) & mask;
        slots_[i] = slot;
    }
}

uint32_t GlyphMeshCache::mesh_for(FontHandle font, uint32_t codepoint, uint32_t tier) {
    if (tier >= kGlyphSizeTierCount) tier = kGlyphSizeTierCount - 1;
    const uint64_t key = key_of(font, codepoint, tier);
    const uint32_t found = find(key);
    if (found != kEmpty) return found;

    const GlyphMeshData mesh = build_glyph_mesh(source_.outline(font, codepoint), source_.units_per_em(font), tier);
    GlyphMeshRange range;
    range.first_vertex = static_cast<uint32_t>(vertices_.size());
    range.first_index  = static_cast<uint32_t>(indices_.size());
    range.index_count  = static_cast<uint32_t>(mesh.indices.size());
    range.advance_em   = mesh.advance_em;
    vertices_.insert(vertices_.end(), mesh.vertices.begin(), mesh.vertices.end());
    indices_.insert(indices_.end(), mesh.indices.begin(), mesh.indices.end());
    const uint32_t id = static_cast<uint32_t>(ranges_.size());
    insert(key, id);
    ranges_.push_back(range);
    return id;
}

}  // namespace pictor
