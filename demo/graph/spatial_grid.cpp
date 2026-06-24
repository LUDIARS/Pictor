#include "spatial_grid.h"

#include <algorithm>
#include <cmath>

#include "graph_store.h"

namespace pictor::graph {

void SpatialGrid::build(const GraphStore& store) {
    const auto& n = store.nodes();
    const size_t count = store.node_count();

    cx_.resize(count);
    cy_.resize(count);
    hw_.resize(count);
    hh_.resize(count);

    if (count == 0) {
        cell_start_.assign(1, 0);
        items_.clear();
        nx_ = ny_ = 1;
        return;
    }

    float min_x = n.cx[0], max_x = n.cx[0];
    float min_y = n.cy[0], max_y = n.cy[0];
    max_hw_ = 0.0f;
    max_hh_ = 0.0f;
    for (size_t i = 0; i < count; ++i) {
        cx_[i] = n.cx[i];
        cy_[i] = n.cy[i];
        hw_[i] = n.w[i] * 0.5f;
        hh_[i] = n.h[i] * 0.5f;
        min_x = std::min(min_x, n.cx[i]);
        max_x = std::max(max_x, n.cx[i]);
        min_y = std::min(min_y, n.cy[i]);
        max_y = std::max(max_y, n.cy[i]);
        max_hw_ = std::max(max_hw_, hw_[i]);
        max_hh_ = std::max(max_hh_, hh_[i]);
    }

    // Aim for roughly 1-2 nodes per cell on average.
    const float span_x = std::max(1.0f, max_x - min_x);
    const float span_y = std::max(1.0f, max_y - min_y);
    const float area    = span_x * span_y;
    const float target  = std::sqrt(area / static_cast<float>(count)); // ~cell for 1/cell
    cell_size_ = std::max(target, std::max(max_hw_, max_hh_) * 2.0f);
    cell_size_ = std::max(cell_size_, 1.0f);

    origin_x_ = min_x;
    origin_y_ = min_y;
    nx_ = std::max(1, static_cast<int>(std::floor(span_x / cell_size_)) + 1);
    ny_ = std::max(1, static_cast<int>(std::floor(span_y / cell_size_)) + 1);
    const int cells = nx_ * ny_;

    // Counting sort into CSR.
    cell_start_.assign(cells + 1, 0);
    std::vector<int> cell_of(count);
    for (size_t i = 0; i < count; ++i) {
        const int c = cell_index(cx_[i], cy_[i]);
        cell_of[i] = c;
        ++cell_start_[c + 1];
    }
    for (int c = 0; c < cells; ++c)
        cell_start_[c + 1] += cell_start_[c];

    items_.resize(count);
    std::vector<uint32_t> cursor(cell_start_.begin(), cell_start_.end() - 1);
    for (size_t i = 0; i < count; ++i) {
        const int c = cell_of[i];
        items_[cursor[c]++] = static_cast<uint32_t>(i);
    }
}

int SpatialGrid::cell_index(float x, float y) const {
    int ix = static_cast<int>(std::floor((x - origin_x_) / cell_size_));
    int iy = static_cast<int>(std::floor((y - origin_y_) / cell_size_));
    ix = std::clamp(ix, 0, nx_ - 1);
    iy = std::clamp(iy, 0, ny_ - 1);
    return iy * nx_ + ix;
}

void SpatialGrid::query_visible(float min_x, float min_y, float max_x, float max_y,
                                std::vector<uint32_t>& out) const {
    out.clear();
    if (cx_.empty()) return;

    // Pad the query so boxes whose center is just outside the view but whose
    // body overlaps still get gathered, then refine per node.
    const float qminx = min_x - max_hw_;
    const float qminy = min_y - max_hh_;
    const float qmaxx = max_x + max_hw_;
    const float qmaxy = max_y + max_hh_;

    int ix0 = static_cast<int>(std::floor((qminx - origin_x_) / cell_size_));
    int iy0 = static_cast<int>(std::floor((qminy - origin_y_) / cell_size_));
    int ix1 = static_cast<int>(std::floor((qmaxx - origin_x_) / cell_size_));
    int iy1 = static_cast<int>(std::floor((qmaxy - origin_y_) / cell_size_));
    ix0 = std::clamp(ix0, 0, nx_ - 1);
    iy0 = std::clamp(iy0, 0, ny_ - 1);
    ix1 = std::clamp(ix1, 0, nx_ - 1);
    iy1 = std::clamp(iy1, 0, ny_ - 1);

    for (int iy = iy0; iy <= iy1; ++iy) {
        for (int ix = ix0; ix <= ix1; ++ix) {
            const int c = iy * nx_ + ix;
            for (uint32_t k = cell_start_[c]; k < cell_start_[c + 1]; ++k) {
                const uint32_t i = items_[k];
                // Exact box-vs-view rect overlap.
                if (cx_[i] + hw_[i] < min_x || cx_[i] - hw_[i] > max_x) continue;
                if (cy_[i] + hh_[i] < min_y || cy_[i] - hh_[i] > max_y) continue;
                out.push_back(i);
            }
        }
    }
}

uint32_t SpatialGrid::pick(float world_x, float world_y) const {
    if (cx_.empty()) return 0xFFFFFFFFu;

    // A node's box can extend max_h* beyond its center cell, so scan a small
    // neighborhood of cells around the point.
    const int rx = static_cast<int>(std::ceil(max_hw_ / cell_size_));
    const int ry = static_cast<int>(std::ceil(max_hh_ / cell_size_));
    int cix = static_cast<int>(std::floor((world_x - origin_x_) / cell_size_));
    int ciy = static_cast<int>(std::floor((world_y - origin_y_) / cell_size_));

    uint32_t best = 0xFFFFFFFFu;
    for (int iy = ciy - ry; iy <= ciy + ry; ++iy) {
        if (iy < 0 || iy >= ny_) continue;
        for (int ix = cix - rx; ix <= cix + rx; ++ix) {
            if (ix < 0 || ix >= nx_) continue;
            const int c = iy * nx_ + ix;
            for (uint32_t k = cell_start_[c]; k < cell_start_[c + 1]; ++k) {
                const uint32_t i = items_[k];
                if (world_x < cx_[i] - hw_[i] || world_x > cx_[i] + hw_[i]) continue;
                if (world_y < cy_[i] - hh_[i] || world_y > cy_[i] + hh_[i]) continue;
                if (best == 0xFFFFFFFFu || i > best) best = i;  // topmost = drawn last
            }
        }
    }
    return best;
}

} // namespace pictor::graph
