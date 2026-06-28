#include "dock_layout.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

namespace pictor::graph {

int DockLayout::add_node(const Node& n) {
    nodes_.push_back(n);
    return static_cast<int>(nodes_.size()) - 1;
}

int DockLayout::build_desc(const DockDesc& desc) {
    switch (desc.kind) {
    case DockDesc::Kind::Leaf: {
        Node n; n.kind = Kind::Leaf; n.panel_id = desc.panel;
        return add_node(n);
    }
    case DockDesc::Kind::Tabs: {
        Node n; n.kind = Kind::Tabs; n.tab_panels = desc.tabs; n.active = desc.active;
        return add_node(n);
    }
    case DockDesc::Kind::Split: {
        const int a = desc.first  ? build_desc(*desc.first)  : -1;
        const int b = desc.second ? build_desc(*desc.second) : -1;
        Node n; n.kind = Kind::Split; n.orient = desc.orient; n.ratio = desc.ratio;
        n.first = a; n.second = b;
        return add_node(n);
    }
    }
    return -1;
}

void DockLayout::set_layout(const DockDesc& root) {
    nodes_.clear();
    root_ = build_desc(root);
}

void DockLayout::set_default(int graph_panel, const std::vector<int>& tab_panels,
                             float split_ratio) {
    const DockDesc right = tab_panels.empty() ? DockDesc::leaf(graph_panel)
                                              : DockDesc::tabset(tab_panels, 0);
    set_layout(DockDesc::split(DockOrient::Horizontal, split_ratio,
                               DockDesc::leaf(graph_panel), right));
}

// ─── Solve ───────────────────────────────────────────────────

void DockLayout::solve(DockRect root) {
    panels_.clear();
    splitters_.clear();
    tabs_.clear();
    if (root_ >= 0) solve_node(root_, root);
}

void DockLayout::solve_node(int idx, DockRect rect) {
    if (idx < 0 || idx >= static_cast<int>(nodes_.size())) return;
    const Node& n = nodes_[idx];

    switch (n.kind) {
    case Kind::Leaf:
        panels_.push_back({n.panel_id, rect});
        break;

    case Kind::Split: {
        SolvedSplitter s;
        s.node = idx;
        s.orient = n.orient;
        s.area = rect;
        if (n.orient == DockOrient::Horizontal) {
            const float avail = std::max(0.0f, rect.w - kSplitter);
            const float fw = avail * n.ratio;
            DockRect first{rect.x, rect.y, fw, rect.h};
            s.handle = {rect.x + fw, rect.y, kSplitter, rect.h};
            DockRect second{rect.x + fw + kSplitter, rect.y, avail - fw, rect.h};
            splitters_.push_back(s);
            solve_node(n.first, first);
            solve_node(n.second, second);
        } else {
            const float avail = std::max(0.0f, rect.h - kSplitter);
            const float fh = avail * n.ratio;
            DockRect first{rect.x, rect.y, rect.w, fh};
            s.handle = {rect.x, rect.y + fh, rect.w, kSplitter};
            DockRect second{rect.x, rect.y + fh + kSplitter, rect.w, avail - fh};
            splitters_.push_back(s);
            solve_node(n.first, first);
            solve_node(n.second, second);
        }
        break;
    }

    case Kind::Tabs: {
        const int count = static_cast<int>(n.tab_panels.size());
        const float bar_h = (count > 0) ? kTabBar : 0.0f;
        const float tab_w = (count > 0) ? rect.w / static_cast<float>(count) : rect.w;
        for (int i = 0; i < count; ++i) {
            SolvedTab t;
            t.node = idx;
            t.panel_id = n.tab_panels[i];
            t.rect = {rect.x + static_cast<float>(i) * tab_w, rect.y, tab_w, bar_h};
            t.active = (i == n.active);
            tabs_.push_back(t);
        }
        DockRect content{rect.x, rect.y + bar_h, rect.w, std::max(0.0f, rect.h - bar_h)};
        if (count > 0) {
            const int active = std::clamp(n.active, 0, count - 1);
            panels_.push_back({n.tab_panels[active], content});
        }
        break;
    }
    }
}

// ─── Interaction ─────────────────────────────────────────────

bool DockLayout::begin_drag(float x, float y) {
    for (const auto& s : splitters_) {
        if (s.handle.contains(x, y)) {
            dragging_ = s.node;
            return true;
        }
    }
    return false;
}

void DockLayout::drag_to(float x, float y) {
    if (dragging_ < 0) return;
    // Use the last solved area of the dragged split node.
    for (const auto& s : splitters_) {
        if (s.node != dragging_) continue;
        float ratio;
        if (s.orient == DockOrient::Horizontal) {
            const float avail = std::max(1.0f, s.area.w - kSplitter);
            ratio = (x - s.area.x) / avail;
        } else {
            const float avail = std::max(1.0f, s.area.h - kSplitter);
            ratio = (y - s.area.y) / avail;
        }
        nodes_[dragging_].ratio = std::clamp(ratio, 0.05f, 0.95f);
        return;
    }
}

bool DockLayout::on_click(float x, float y) {
    for (const auto& t : tabs_) {
        if (!t.rect.contains(x, y)) continue;
        Node& node = nodes_[t.node];
        for (int i = 0; i < static_cast<int>(node.tab_panels.size()); ++i) {
            if (node.tab_panels[i] == t.panel_id) {
                node.active = i;
                return true;
            }
        }
    }
    return false;
}

// ─── Persistence (simple line format) ────────────────────────

void DockLayout::save(const char* path) const {
    std::ofstream out(path, std::ios::trunc);
    if (!out.is_open()) return;
    out << "dock 1\n";
    out << "root " << root_ << "\n";
    out << "nodes " << nodes_.size() << "\n";
    for (const auto& n : nodes_) {
        if (n.kind == Kind::Leaf) {
            out << "leaf " << n.panel_id << "\n";
        } else if (n.kind == Kind::Split) {
            out << "split " << (n.orient == DockOrient::Horizontal ? 'H' : 'V')
                << ' ' << n.ratio << ' ' << n.first << ' ' << n.second << "\n";
        } else {
            out << "tabs " << n.active << ' ' << n.tab_panels.size();
            for (int p : n.tab_panels) out << ' ' << p;
            out << "\n";
        }
    }
}

bool DockLayout::load(const char* path) {
    std::ifstream in(path);
    if (!in.is_open()) return false;

    std::vector<Node> parsed;
    int root = -1;
    size_t expected = 0;
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream ss(line);
        std::string tag;
        ss >> tag;
        if (tag == "dock") {
            // version — ignore for now
        } else if (tag == "root") {
            ss >> root;
        } else if (tag == "nodes") {
            ss >> expected;
        } else if (tag == "leaf") {
            Node n; n.kind = Kind::Leaf; ss >> n.panel_id; parsed.push_back(n);
        } else if (tag == "split") {
            Node n; n.kind = Kind::Split;
            char o = 'H';
            ss >> o >> n.ratio >> n.first >> n.second;
            n.orient = (o == 'V') ? DockOrient::Vertical : DockOrient::Horizontal;
            parsed.push_back(n);
        } else if (tag == "tabs") {
            Node n; n.kind = Kind::Tabs;
            size_t cnt = 0;
            ss >> n.active >> cnt;
            for (size_t i = 0; i < cnt; ++i) { int p = 0; ss >> p; n.tab_panels.push_back(p); }
            parsed.push_back(n);
        }
    }

    if (parsed.empty() || root < 0 || root >= static_cast<int>(parsed.size()))
        return false;
    if (expected != parsed.size())
        return false;  // corrupt / partial file — fall back to default

    nodes_ = std::move(parsed);
    root_  = root;
    return true;
}

} // namespace pictor::graph
