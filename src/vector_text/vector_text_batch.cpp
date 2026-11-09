#include "pictor/vector_text/vector_text_batch.h"

#include <algorithm>
#include <numeric>

namespace pictor {

void VectorTextBatch::clear() {
    instances_.clear();
    meshes_.clear();
    draws_.clear();
}

void VectorTextBatch::add(uint32_t mesh, const VectorTextInstance& instance) {
    meshes_.push_back(mesh);
    instances_.push_back(instance);
}

void VectorTextBatch::build_draws() {
    draws_.clear();
    const size_t n = instances_.size();
    if (n == 0) return;
    order_.resize(n);
    std::iota(order_.begin(), order_.end(), 0U);
    std::stable_sort(order_.begin(), order_.end(), [&](uint32_t a, uint32_t b) { return meshes_[a] < meshes_[b]; });
    sorted_instances_.resize(n);
    sorted_meshes_.resize(n);
    for (size_t i = 0; i < n; ++i) {
        sorted_instances_[i] = instances_[order_[i]];
        sorted_meshes_[i]    = meshes_[order_[i]];
    }
    instances_.swap(sorted_instances_);
    meshes_.swap(sorted_meshes_);
    for (uint32_t i = 0; i < n; ++i) {
        if (draws_.empty() || draws_.back().mesh != meshes_[i]) {
            draws_.push_back({meshes_[i], i, 0});
        }
        ++draws_.back().instance_count;
    }
}

}  // namespace pictor
