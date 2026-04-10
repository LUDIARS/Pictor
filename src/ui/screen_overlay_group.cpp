#include "pictor/ui/screen_overlay_group.h"
#include <algorithm>
#include <cstdio>
#include <cmath>

namespace pictor {

ScreenOverlayGroup::ScreenOverlayGroup(OverlayGroupId id, const std::string& name)
    : id_(id), name_(name.empty() ? "group_" + std::to_string(id) : name) {}

// ─── Element management ──────────────────────────────────────

OverlayElementId ScreenOverlayGroup::add_element(const OverlayElement& elem) {
    OverlayElement e = elem;
    e.id = next_element_id_++;
    elements_.push_back(e);
    dirty_ = true;
    return e.id;
}

bool ScreenOverlayGroup::update_element(OverlayElementId id, const OverlayElement& elem) {
    for (auto& e : elements_) {
        if (e.id == id) {
            bool was_static = e.is_static;
            e = elem;
            e.id = id;  // preserve ID
            if (was_static || e.is_static) dirty_ = true;
            return true;
        }
    }
    return false;
}

bool ScreenOverlayGroup::remove_element(OverlayElementId id) {
    auto it = std::find_if(elements_.begin(), elements_.end(),
                           [id](const OverlayElement& e) { return e.id == id; });
    if (it == elements_.end()) return false;
    if (it->is_static) dirty_ = true;
    elements_.erase(it);
    return true;
}

const OverlayElement* ScreenOverlayGroup::get_element(OverlayElementId id) const {
    for (const auto& e : elements_) {
        if (e.id == id) return &e;
    }
    return nullptr;
}

void ScreenOverlayGroup::clear() {
    elements_.clear();
    static_vertices_.clear();
    combined_vertices_.clear();
    dirty_ = true;
}

// ─── Batch rebuild ───────────────────────────────────────────

bool ScreenOverlayGroup::rebuild_if_dirty(float screen_width, float screen_height) {
    // Dynamic elements always need regeneration
    bool has_dynamic = false;
    for (const auto& e : elements_) {
        if (!e.is_static && e.visible) { has_dynamic = true; break; }
    }

    if (!dirty_ && !has_dynamic) return false;

    // Sort elements by z_order
    std::vector<const OverlayElement*> sorted;
    sorted.reserve(elements_.size());
    for (const auto& e : elements_) {
        if (e.visible) sorted.push_back(&e);
    }
    std::sort(sorted.begin(), sorted.end(),
              [](const OverlayElement* a, const OverlayElement* b) {
                  return a->z_order < b->z_order;
              });

    // Rebuild static vertices only when dirty
    if (dirty_) {
        static_vertices_.clear();
        for (const auto* e : sorted) {
            if (e->is_static) {
                generate_quad_vertices(*e, screen_width, screen_height, static_vertices_);
            }
        }
        rebuild_count_++;
    }

    // Build combined: static batch + dynamic elements
    combined_vertices_ = static_vertices_;
    for (const auto* e : sorted) {
        if (!e->is_static) {
            generate_quad_vertices(*e, screen_width, screen_height, combined_vertices_);
        }
    }

    dirty_ = false;
    return true;
}

// ─── Quad vertex generation ──────────────────────────────────

void ScreenOverlayGroup::generate_quad_vertices(const OverlayElement& elem,
                                                  float screen_w, float screen_h,
                                                  std::vector<UIVertex>& out) const {
    // Convert screen pixels to NDC [-1, 1]
    // NDC x: left=-1, right=+1
    // NDC y: top=-1, bottom=+1 (Vulkan convention)
    auto to_ndc_x = [screen_w](float px) { return (px / screen_w) * 2.0f - 1.0f; };
    auto to_ndc_y = [screen_h](float py) { return (py / screen_h) * 2.0f - 1.0f; };

    // Quad corners relative to center (in pixels)
    float hw = elem.width * 0.5f;
    float hh = elem.height * 0.5f;

    struct V2 { float x, y; };
    V2 corners[4] = {
        {-hw, -hh},  // top-left
        { hw, -hh},  // top-right
        { hw,  hh},  // bottom-right
        {-hw,  hh},  // bottom-left
    };

    // Apply rotation
    if (elem.rotation != 0.0f) {
        float rad = elem.rotation * 3.14159265f / 180.0f;
        float c = std::cos(rad);
        float s = std::sin(rad);
        for (auto& v : corners) {
            float rx = v.x * c - v.y * s;
            float ry = v.x * s + v.y * c;
            v.x = rx;
            v.y = ry;
        }
    }

    // Translate to element position (center) and convert to NDC
    float cx = elem.x + hw;
    float cy = elem.y + hh;

    for (auto& v : corners) {
        v.x = to_ndc_x(cx + v.x);
        v.y = to_ndc_y(cy + v.y);
    }

    // UV corners
    float u0 = elem.uv_min[0], v0 = elem.uv_min[1];
    float u1 = elem.uv_max[0], v1 = elem.uv_max[1];

    const float* col = elem.color;

    // Two triangles (6 vertices)
    UIVertex verts[6] = {
        {{corners[0].x, corners[0].y}, {u0, v0}, {col[0], col[1], col[2], col[3]}},
        {{corners[1].x, corners[1].y}, {u1, v0}, {col[0], col[1], col[2], col[3]}},
        {{corners[2].x, corners[2].y}, {u1, v1}, {col[0], col[1], col[2], col[3]}},
        {{corners[0].x, corners[0].y}, {u0, v0}, {col[0], col[1], col[2], col[3]}},
        {{corners[2].x, corners[2].y}, {u1, v1}, {col[0], col[1], col[2], col[3]}},
        {{corners[3].x, corners[3].y}, {u0, v1}, {col[0], col[1], col[2], col[3]}},
    };

    out.insert(out.end(), std::begin(verts), std::end(verts));
}

} // namespace pictor
