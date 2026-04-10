#pragma once

#include "pictor/ui/overlay_types.h"
#include <vector>
#include <string>
#include <cmath>

namespace pictor {

/// A named group of overlay elements.
/// Static elements are batched into a single vertex array.
/// When elements change, the batch is marked dirty and rebuilt on next render.
class ScreenOverlayGroup {
public:
    explicit ScreenOverlayGroup(OverlayGroupId id, const std::string& name = "");

    // ─── Element management ──────────────────────────────────

    /// Add an element and return its ID. Marks static batch as dirty.
    OverlayElementId add_element(const OverlayElement& elem);

    /// Update an existing element. Marks batch dirty if element is static.
    bool update_element(OverlayElementId id, const OverlayElement& elem);

    /// Remove an element. Marks batch dirty if element was static.
    bool remove_element(OverlayElementId id);

    /// Get element by ID (nullptr if not found)
    const OverlayElement* get_element(OverlayElementId id) const;

    /// Clear all elements
    void clear();

    // ─── Batch management ────────────────────────────────────

    /// Rebuild the batched vertex data if dirty. Returns true if rebuilt.
    bool rebuild_if_dirty(float screen_width, float screen_height);

    /// Force rebuild
    void mark_dirty() { dirty_ = true; }

    /// Get the batched vertex data (static + dynamic combined)
    const std::vector<UIVertex>& vertices() const { return combined_vertices_; }
    uint32_t vertex_count() const { return static_cast<uint32_t>(combined_vertices_.size()); }

    // ─── Properties ──────────────────────────────────────────

    OverlayGroupId id() const { return id_; }
    const std::string& name() const { return name_; }
    bool is_visible() const { return visible_; }
    void set_visible(bool v) { visible_ = v; }
    int32_t sort_order() const { return sort_order_; }
    void set_sort_order(int32_t order) { sort_order_ = order; }

    /// Texture used by this group (all elements in a group share one texture for batching)
    TextureHandle texture() const { return texture_; }
    void set_texture(TextureHandle tex) { texture_ = tex; }

    uint32_t element_count() const { return static_cast<uint32_t>(elements_.size()); }

private:
    void generate_quad_vertices(const OverlayElement& elem,
                                 float screen_w, float screen_h,
                                 std::vector<UIVertex>& out) const;

    OverlayGroupId id_;
    std::string name_;
    bool visible_ = true;
    int32_t sort_order_ = 0;
    TextureHandle texture_ = INVALID_TEXTURE;

    std::vector<OverlayElement> elements_;
    OverlayElementId next_element_id_ = 0;

    // Cached batch data
    std::vector<UIVertex> static_vertices_;    // static elements only
    std::vector<UIVertex> combined_vertices_;  // static + dynamic
    bool dirty_ = true;
    uint32_t rebuild_count_ = 0;
};

} // namespace pictor
