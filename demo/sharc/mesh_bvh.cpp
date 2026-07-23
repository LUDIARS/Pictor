#include "mesh_bvh.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace sharc_demo {

namespace {

constexpr uint32_t kLeafSize = 8;
constexpr int      kMaxDepth = 40;

void tri_bounds(const PlyMesh& mesh, uint32_t tri, float bmin[3],
                float bmax[3]) {
    const auto& t = mesh.triangles[tri];
    for (int a = 0; a < 3; ++a) {
        bmin[a] = 1e30f;
        bmax[a] = -1e30f;
    }
    for (uint32_t vi : t) {
        const auto& p = mesh.positions[vi];
        for (int a = 0; a < 3; ++a) {
            bmin[a] = std::fmin(bmin[a], p[a]);
            bmax[a] = std::fmax(bmax[a], p[a]);
        }
    }
}

float tri_centroid_axis(const PlyMesh& mesh, uint32_t tri, int axis) {
    const auto& t = mesh.triangles[tri];
    return (mesh.positions[t[0]][axis] + mesh.positions[t[1]][axis] +
            mesh.positions[t[2]][axis]) * (1.0f / 3.0f);
}

bool aabb_hit(const float bmin[3], const float bmax[3], const float ro[3],
              const float inv_rd[3], float tmax) {
    float t0 = 0.0f, t1 = tmax;
    for (int a = 0; a < 3; ++a) {
        float lo = (bmin[a] - ro[a]) * inv_rd[a];
        float hi = (bmax[a] - ro[a]) * inv_rd[a];
        if (inv_rd[a] < 0.0f) std::swap(lo, hi);
        t0 = std::fmax(t0, lo);
        t1 = std::fmin(t1, hi);
        if (t0 > t1) return false;
    }
    return true;
}

} // namespace

void MeshBvh::build(const PlyMesh& mesh) {
    mesh_ = &mesh;
    nodes_.clear();
    tri_order_.resize(mesh.triangles.size());
    for (uint32_t i = 0; i < tri_order_.size(); ++i) tri_order_[i] = i;
    if (tri_order_.empty()) return;
    nodes_.reserve(tri_order_.size() / 2);
    build_recursive_(tri_order_, 0, static_cast<uint32_t>(tri_order_.size()),
                     0);
    std::fprintf(stderr, "[bvh] built: %zu nodes for %zu tris\n",
                 nodes_.size(), mesh.triangles.size());
}

uint32_t MeshBvh::build_recursive_(std::vector<uint32_t>& tris, uint32_t begin,
                                   uint32_t end, int depth) {
    const uint32_t node_index = static_cast<uint32_t>(nodes_.size());
    nodes_.push_back(Node{});

    // AABB
    float bmin[3] = {1e30f, 1e30f, 1e30f};
    float bmax[3] = {-1e30f, -1e30f, -1e30f};
    for (uint32_t i = begin; i < end; ++i) {
        float tmin[3], tmax_[3];
        tri_bounds(*mesh_, tris[i], tmin, tmax_);
        for (int a = 0; a < 3; ++a) {
            bmin[a] = std::fmin(bmin[a], tmin[a]);
            bmax[a] = std::fmax(bmax[a], tmax_[a]);
        }
    }

    const uint32_t count = end - begin;
    if (count <= kLeafSize || depth >= kMaxDepth) {
        Node& leaf = nodes_[node_index];
        std::copy(bmin, bmin + 3, leaf.bmin);
        std::copy(bmax, bmax + 3, leaf.bmax);
        leaf.left  = begin;
        leaf.count = count;
        return node_index;
    }

    // 最長軸を重心の中央値で分割
    int axis = 0;
    float extent = bmax[0] - bmin[0];
    for (int a = 1; a < 3; ++a) {
        const float e = bmax[a] - bmin[a];
        if (e > extent) { extent = e; axis = a; }
    }
    const uint32_t mid = begin + count / 2;
    std::nth_element(tris.begin() + begin, tris.begin() + mid,
                     tris.begin() + end,
                     [this, axis](uint32_t a, uint32_t b) {
                         return tri_centroid_axis(*mesh_, a, axis) <
                                tri_centroid_axis(*mesh_, b, axis);
                     });

    const uint32_t left  = build_recursive_(tris, begin, mid, depth + 1);
    const uint32_t right = build_recursive_(tris, mid, end, depth + 1);
    (void)left;   // left は常に node_index+1 — right だけ格納すれば復元可能
    Node& node = nodes_[node_index];
    std::copy(bmin, bmin + 3, node.bmin);
    std::copy(bmax, bmax + 3, node.bmax);
    node.left  = right;   // 内部ノード: left 子 = node_index+1, right 子 = この値
    node.count = 0;
    return node_index;
}

BvhHit MeshBvh::intersect(const float ro[3], const float rd[3],
                          float tmax) const {
    BvhHit hit;
    if (nodes_.empty()) return hit;

    const float inv_rd[3] = {
        1.0f / (std::fabs(rd[0]) > 1e-12f ? rd[0] : 1e-12f),
        1.0f / (std::fabs(rd[1]) > 1e-12f ? rd[1] : 1e-12f),
        1.0f / (std::fabs(rd[2]) > 1e-12f ? rd[2] : 1e-12f),
    };

    uint32_t stack[64];
    int sp = 0;
    stack[sp++] = 0;
    float best = tmax;

    while (sp > 0) {
        const Node& node = nodes_[stack[--sp]];
        if (!aabb_hit(node.bmin, node.bmax, ro, inv_rd, best)) continue;

        if (node.count > 0) {
            // 葉: Möller–Trumbore
            for (uint32_t i = 0; i < node.count; ++i) {
                const uint32_t tri = tri_order_[node.left + i];
                const auto& idx = mesh_->triangles[tri];
                const auto& p0 = mesh_->positions[idx[0]];
                const auto& p1 = mesh_->positions[idx[1]];
                const auto& p2 = mesh_->positions[idx[2]];

                const float e1[3] = {p1[0] - p0[0], p1[1] - p0[1],
                                     p1[2] - p0[2]};
                const float e2[3] = {p2[0] - p0[0], p2[1] - p0[1],
                                     p2[2] - p0[2]};
                const float pv[3] = {rd[1] * e2[2] - rd[2] * e2[1],
                                     rd[2] * e2[0] - rd[0] * e2[2],
                                     rd[0] * e2[1] - rd[1] * e2[0]};
                const float det = e1[0] * pv[0] + e1[1] * pv[1] +
                                  e1[2] * pv[2];
                if (std::fabs(det) < 1e-12f) continue;
                const float inv_det = 1.0f / det;
                const float tv[3] = {ro[0] - p0[0], ro[1] - p0[1],
                                     ro[2] - p0[2]};
                const float u = (tv[0] * pv[0] + tv[1] * pv[1] +
                                 tv[2] * pv[2]) * inv_det;
                if (u < 0.0f || u > 1.0f) continue;
                const float qv[3] = {tv[1] * e1[2] - tv[2] * e1[1],
                                     tv[2] * e1[0] - tv[0] * e1[2],
                                     tv[0] * e1[1] - tv[1] * e1[0]};
                const float v = (rd[0] * qv[0] + rd[1] * qv[1] +
                                 rd[2] * qv[2]) * inv_det;
                if (v < 0.0f || u + v > 1.0f) continue;
                const float t = (e2[0] * qv[0] + e2[1] * qv[1] +
                                 e2[2] * qv[2]) * inv_det;
                if (t <= 1e-4f || t >= best) continue;

                best = t;
                hit.t = t;
                const float w = 1.0f - u - v;
                const auto& n0 = mesh_->normals[idx[0]];
                const auto& n1 = mesh_->normals[idx[1]];
                const auto& n2 = mesh_->normals[idx[2]];
                float n[3] = {w * n0[0] + u * n1[0] + v * n2[0],
                              w * n0[1] + u * n1[1] + v * n2[1],
                              w * n0[2] + u * n1[2] + v * n2[2]};
                const float l = std::sqrt(n[0] * n[0] + n[1] * n[1] +
                                          n[2] * n[2]);
                if (l > 1e-12f) {
                    n[0] /= l; n[1] /= l; n[2] /= l;
                }
                // 裏面ヒット (逆光透過で頻出) は視線側へ向ける
                const float facing = n[0] * rd[0] + n[1] * rd[1] +
                                     n[2] * rd[2];
                if (facing > 0.0f) {
                    n[0] = -n[0]; n[1] = -n[1]; n[2] = -n[2];
                }
                std::copy(n, n + 3, hit.normal);
            }
        } else {
            // 内部: 左子 = 自分+1 だが index を直接持たないため復元
            const uint32_t self =
                static_cast<uint32_t>(&node - nodes_.data());
            if (sp + 2 <= 64) {
                stack[sp++] = node.left;     // right
                stack[sp++] = self + 1;      // left (直後に配置される規約)
            }
        }
    }
    return hit;
}

}  // namespace sharc_demo
