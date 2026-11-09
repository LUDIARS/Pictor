#pragma once

#include "pictor/vector_text/glyph_mesh.h"
#include "pictor/vector_text/glyph_outline_source.h"

#include <cstdint>
#include <vector>

namespace pictor {

/// Builds each (font, codepoint, size tier) mesh once and keeps all of them in
/// two flat arrays that a backend uploads as single vertex / index buffers.
///
/// Lookup is an open-addressing table over a flat vector (no node allocation).
/// The arrays only grow, so a backend uploads just the tail added since its
/// last upload.
class GlyphMeshCache {
public:
    explicit GlyphMeshCache(const IGlyphOutlineSource& source) : source_(source) {}

    GlyphMeshCache(const GlyphMeshCache&) = delete;
    GlyphMeshCache& operator=(const GlyphMeshCache&) = delete;

    /// Id of the mesh (built on first use). Throws std::invalid_argument when
    /// the outline is malformed.
    uint32_t mesh_for(FontHandle font, uint32_t codepoint, uint32_t tier);

    [[nodiscard]] const GlyphMeshRange& range(uint32_t mesh) const { return ranges_[mesh]; }
    [[nodiscard]] uint32_t mesh_count() const { return static_cast<uint32_t>(ranges_.size()); }
    [[nodiscard]] const std::vector<GlyphVertex>& vertices() const { return vertices_; }
    [[nodiscard]] const std::vector<uint32_t>& indices() const { return indices_; }
    [[nodiscard]] const IGlyphOutlineSource& source() const { return source_; }

private:
    static constexpr uint32_t kEmpty = UINT32_MAX;
    struct Slot {
        uint64_t key  = 0;
        uint32_t mesh = kEmpty;
    };

    static uint64_t key_of(FontHandle font, uint32_t codepoint, uint32_t tier);
    [[nodiscard]] uint32_t find(uint64_t key) const;
    void insert(uint64_t key, uint32_t mesh);
    void grow_table();

    const IGlyphOutlineSource&  source_;
    std::vector<Slot>           slots_;
    std::vector<GlyphMeshRange> ranges_;
    std::vector<GlyphVertex>    vertices_;
    std::vector<uint32_t>       indices_;
};

} // namespace pictor
