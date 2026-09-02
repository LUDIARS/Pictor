#include "eye_locator.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace pictor_fbx_viewer {

namespace {

struct Blob {
    double sum_x = 0, sum_y = 0;
    uint32_t area = 0;
};

/// @implements SPEC-FBX-VIEWER-FUR-EFFECTS
bool is_dark(const uint8_t* px) {
    // Luminance well below the plush's white body; ignore transparent texels.
    const float lum = (0.299f * px[0] + 0.587f * px[1] + 0.114f * px[2]) / 255.0f;
    return px[3] > 128 && lum < 0.16f;
}

} // namespace

/// @implements SPEC-FBX-VIEWER-FUR-EFFECTS
std::vector<EyeAnchor> locate_eyes(const uint8_t* rgba, int w, int h,
                                   const std::vector<TexturedSkinnedVertex>& vertices,
                                   const std::vector<uint32_t>& indices,
                                   uint32_t index_start,
                                   uint32_t index_count) {
    std::vector<EyeAnchor> out;
    if (!rgba || w <= 0 || h <= 0 || vertices.empty() || indices.empty()) return out;

    const size_t pixel_count = static_cast<size_t>(w) * static_cast<size_t>(h);
    if (pixel_count > UINT32_MAX) return out;
    const size_t first_index = std::min<size_t>(index_start, indices.size());
    const size_t available = indices.size() - first_index;
    const size_t last_index = first_index + std::min<size_t>(index_count, available);
    if (first_index == last_index) return out;

    // Flood fill dark texels into blobs (4-connectivity, iterative stack).
    std::vector<uint8_t> visited(pixel_count, 0);
    std::vector<Blob> blobs;
    std::vector<uint32_t> stack;
    const uint32_t max_area = static_cast<uint32_t>(static_cast<double>(w) * h * 0.02);   // eyes are small
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const size_t i = static_cast<size_t>(y) * w + x;
            if (visited[i] || !is_dark(rgba + i * 4)) continue;
            Blob b;
            stack.clear();
            stack.push_back(static_cast<uint32_t>(i));
            visited[i] = 1;
            while (!stack.empty()) {
                const uint32_t p = stack.back(); stack.pop_back();
                const int px = static_cast<int>(p % w), py = static_cast<int>(p / w);
                b.sum_x += px; b.sum_y += py; ++b.area;
                const int nx[4] = {px - 1, px + 1, px, px};
                const int ny[4] = {py, py, py - 1, py + 1};
                for (int k = 0; k < 4; ++k) {
                    if (nx[k] < 0 || ny[k] < 0 || nx[k] >= w || ny[k] >= h) continue;
                    const size_t j = static_cast<size_t>(ny[k]) * w + nx[k];
                    if (visited[j] || !is_dark(rgba + j * 4)) continue;
                    visited[j] = 1;
                    stack.push_back(static_cast<uint32_t>(j));
                }
            }
            if (b.area >= 12 && b.area <= max_area) blobs.push_back(b);
        }
    }
    std::sort(blobs.begin(), blobs.end(), [](const Blob& a, const Blob& b) { return a.area > b.area; });

    // Take the two largest blobs that are clearly apart in UV (the mouth dot
    // is smaller than either eye and sits between them).
    std::vector<std::pair<float, float>> centres;
    for (const Blob& b : blobs) {
        const float u = static_cast<float>(b.sum_x / b.area) / w;
        const float v = static_cast<float>(b.sum_y / b.area) / h;
        bool apart = true;
        for (const auto& c : centres) {
            if (std::hypot(c.first - u, c.second - v) < 0.02f) { apart = false; break; }
        }
        if (!apart) continue;
        centres.push_back({u, v});
        EyeAnchor a;
        a.uv[0] = u; a.uv[1] = v; a.blob_area_px = static_cast<float>(b.area);
        out.push_back(a);
        if (out.size() == 2) break;
    }

    for (EyeAnchor& a : out) {
        float best = 1e9f;
        for (size_t i = first_index; i < last_index; ++i) {
            const uint32_t vertex_index = indices[i];
            if (vertex_index >= vertices.size()) continue;
            const float du = vertices[vertex_index].uv[0] - a.uv[0];
            const float dv = vertices[vertex_index].uv[1] - a.uv[1];
            const float d = du * du + dv * dv;
            if (d < best) {
                best = d;
                a.vertex_index = vertex_index;
                a.uv_distance_sq = d;
            }
        }
        if (a.vertex_index == UINT32_MAX) continue;
        std::printf("[eyes] blob uv=(%.3f,%.3f) area=%.0f -> vertex %u (uv dist %.4f)\n",
                    a.uv[0], a.uv[1], a.blob_area_px, a.vertex_index, std::sqrt(best));
    }
    out.erase(std::remove_if(out.begin(), out.end(), [&](const EyeAnchor& a) {
        return a.vertex_index >= vertices.size();
    }), out.end());
    return out;
}

} // namespace pictor_fbx_viewer
