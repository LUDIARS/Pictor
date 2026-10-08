#include "morph_targets.h"

#include <cstring>

namespace pictor_fbx_viewer {

namespace {
// Deltas below this length (model units) are FBX export noise, not shape data.
constexpr float kDeltaEpsilon = 1e-7f;
}

uint32_t MorphTargetsBuilder::slot_for(uint32_t vertex, const float* positions, size_t stride_floats) {
    const auto [it, added] = slot_of_vertex_.emplace(vertex, static_cast<uint32_t>(slot_vertex_.size()));
    if (added) {
        slot_vertex_.push_back(vertex);
        const float* p = positions + static_cast<size_t>(vertex) * stride_floats;
        slot_base_.insert(slot_base_.end(), p, p + 3);
    }
    return it->second;
}

/// @implements SPEC-PC-FBX-TRACK-PLAYBACK
void MorphTargetsBuilder::add_shape(const std::string& name, uint32_t vertex_base, uint32_t vertex_count,
                                    const float* deltas, const float* positions, size_t stride_floats) {
    size_t shape = names_.size();
    for (size_t i = 0; i < names_.size(); ++i) {
        if (names_[i] == name) { shape = i; break; }
    }
    if (shape == names_.size()) {
        names_.push_back(name);
        shapes_.emplace_back();
    }
    Pending& dst = shapes_[shape];
    for (uint32_t v = 0; v < vertex_count; ++v) {
        const float* d = deltas + static_cast<size_t>(v) * 3;
        if (d[0] * d[0] + d[1] * d[1] + d[2] * d[2] <= kDeltaEpsilon * kDeltaEpsilon) continue;
        dst.slot.push_back(slot_for(vertex_base + v, positions, stride_floats));
        dst.delta.insert(dst.delta.end(), d, d + 3);
    }
}

/// @implements SPEC-PC-FBX-TRACK-PLAYBACK
MorphTargets MorphTargetsBuilder::build() {
    MorphTargets out;
    out.names = std::move(names_);
    out.shape_begin.reserve(shapes_.size() + 1);
    out.shape_begin.push_back(0);
    for (const Pending& s : shapes_) {
        out.entry_slot.insert(out.entry_slot.end(), s.slot.begin(), s.slot.end());
        out.entry_delta.insert(out.entry_delta.end(), s.delta.begin(), s.delta.end());
        out.shape_begin.push_back(static_cast<uint32_t>(out.entry_slot.size()));
    }
    out.slot_vertex = std::move(slot_vertex_);
    out.slot_base   = std::move(slot_base_);
    *this = MorphTargetsBuilder{};
    return out;
}

void MorphApplier::bind(const MorphTargets* targets) {
    targets_ = targets;
    accum_.assign(targets ? targets->slot_base.size() : 0, 0.0f);
}

/// @implements SPEC-PC-FBX-TRACK-PLAYBACK
void MorphApplier::apply(const float* weights, float* positions, size_t stride_floats) {
    if (!targets_ || accum_.empty()) return;
    const MorphTargets& t = *targets_;
    // Re-base every touched vertex from its original position.
    std::memcpy(accum_.data(), t.slot_base.data(), accum_.size() * sizeof(float));
    for (uint32_t c = 0; c < t.channel_count(); ++c) {
        const float w = weights[c];
        if (!(w > 0.0f)) continue;
        for (uint32_t e = t.shape_begin[c]; e < t.shape_begin[c + 1]; ++e) {
            float* a = accum_.data() + static_cast<size_t>(t.entry_slot[e]) * 3;
            const float* d = t.entry_delta.data() + static_cast<size_t>(e) * 3;
            a[0] += w * d[0];
            a[1] += w * d[1];
            a[2] += w * d[2];
        }
    }
    const size_t slots = t.slot_vertex.size();
    for (size_t s = 0; s < slots; ++s) {
        float* p = positions + static_cast<size_t>(t.slot_vertex[s]) * stride_floats;
        p[0] = accum_[s * 3 + 0];
        p[1] = accum_[s * 3 + 1];
        p[2] = accum_[s * 3 + 2];
    }
}

} // namespace pictor_fbx_viewer
