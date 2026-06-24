#pragma once

#include <cstdint>
#include <vector>

namespace pictor::graph {

/// Float rect in window pixels (Vulkan-free so the layout is self-contained).
struct DockRect {
    float x = 0, y = 0, w = 0, h = 0;
    bool contains(float px, float py) const {
        return px >= x && px < x + w && py >= y && py < y + h;
    }
};

enum class DockOrient : uint8_t { Horizontal, Vertical }; // H = side-by-side, V = stacked

/// Native dock layouter: a split / tabs / leaf tree that arranges panels, with
/// draggable splitters, clickable tab headers and text persistence. Mirrors the
/// semantics of Corpus's DockComponent (DockLayoutNode: leaf / split / tabs).
/// Panels are referenced by integer id; the host decides what each id renders.
class DockLayout {
public:
    struct SolvedPanel   { int   panel_id; DockRect rect; };               // visible content
    struct SolvedSplitter{ int   node;     DockOrient orient; DockRect handle; DockRect area; };
    struct SolvedTab     { int   node;     int panel_id; DockRect rect; bool active; };

    /// Build the default layout: `graph` leaf on the left, a tabs container with
    /// `tab_panels` on the right.
    void set_default(int graph_panel, const std::vector<int>& tab_panels, float split_ratio = 0.72f);

    bool load(const char* path);
    void save(const char* path) const;

    /// Recompute geometry for the given root rect; results are cached for both
    /// the host (panels()/splitters()/tabs()) and interaction.
    void solve(DockRect root);

    const std::vector<SolvedPanel>&    panels()    const { return panels_; }
    const std::vector<SolvedSplitter>& splitters() const { return splitters_; }
    const std::vector<SolvedTab>&      tabs()      const { return tabs_; }

    bool begin_drag(float x, float y);   // grab a splitter; true if one was hit
    void drag_to(float x, float y);      // adjust the dragged split's ratio
    void end_drag() { dragging_ = -1; }
    bool is_dragging() const { return dragging_ >= 0; }

    bool on_click(float x, float y);     // tab switch; true if consumed

    static constexpr float kSplitter = 6.0f;
    static constexpr float kTabBar   = 26.0f;

private:
    enum class Kind : uint8_t { Leaf, Split, Tabs };

    struct Node {
        Kind kind = Kind::Leaf;
        int  panel_id = 0;          // Leaf
        DockOrient orient = DockOrient::Horizontal; // Split
        float ratio = 0.5f;         // Split
        int  first = -1, second = -1;               // Split children
        std::vector<int> tab_panels;                // Tabs
        int  active = 0;            // Tabs (index into tab_panels)
    };

    int  add_node(const Node& n);
    void solve_node(int idx, DockRect rect);

    std::vector<Node> nodes_;
    int               root_ = -1;

    std::vector<SolvedPanel>    panels_;
    std::vector<SolvedSplitter> splitters_;
    std::vector<SolvedTab>      tabs_;

    int dragging_ = -1; // node index of the split being dragged, or -1
};

} // namespace pictor::graph
