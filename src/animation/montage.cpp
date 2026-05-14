#include "pictor/animation/montage.h"

#include "pictor/animation/animation_clip.h"
#include "pictor/animation/animation_system.h"
#include "pictor/animation/skeleton.h"

#include <algorithm>
#include <cstdio>

namespace pictor {

MontagePlayer::MontagePlayer(AnimationSystem& sys) : sys_(sys) {}
MontagePlayer::~MontagePlayer() = default;

MontageHandle MontagePlayer::register_montage(const MontageDescriptor& desc) {
    MontageHandle h{static_cast<uint32_t>(montages_.size() + 1)};
    montages_.push_back(desc);
    return h;
}

void MontagePlayer::rebuild_remap_(AnimationClipHandle clip, SkeletonHandle skel) {
    PairKey key{clip, skel};
    if (remap_cache_.count(key)) return;
    const AnimationClip* c = sys_.get_clip(clip);
    const Skeleton*      s = sys_.get_skeleton(skel);
    if (!c || !s || c->bone_names().empty()) {
        remap_cache_[key] = {};
        return;
    }
    std::vector<int> remap(c->channels().size(), -1);
    for (std::size_t i = 0; i < c->channels().size(); ++i) {
        if (i >= c->bone_names().size()) continue;
        const std::string& bone_name = c->bone_names()[i];
        if (bone_name.empty()) continue;
        // Skeleton::find_bone(name) を使う
        const int32_t idx = s->find_bone(bone_name);
        if (idx >= 0) remap[i] = idx;
    }
    remap_cache_[key] = std::move(remap);
}

const std::vector<int>* MontagePlayer::get_bone_remap(AnimationClipHandle clip, SkeletonHandle skel) const {
    PairKey key{clip, skel};
    auto it = remap_cache_.find(key);
    return it == remap_cache_.end() ? nullptr : &it->second;
}

void MontagePlayer::play(AnimationStateHandle inst, MontageHandle m,
                         SkeletonHandle skel, const MontagePlayParams& params) {
    if (m.id == 0 || m.id > montages_.size()) return;
    const MontageDescriptor& d = montages_[m.id - 1];
    rebuild_remap_(d.clip, skel);

    auto exit = actives_.find(inst);
    if (exit != actives_.end()) actives_.erase(exit);

    const float init_weight = params.blend_in_time > 0.0f ? 0.0f : params.weight;
    sys_.play(inst, d.clip, init_weight, params.speed, params.blend);

    // remap テーブルが構築済みなら、 直前に push された AnimationPlayback layer に
    // channel_remap ポインタを差し込む。 AnimationSystem 内部で push_back された
    // 最後尾 layer を再取得して書き換える。 失敗しても legacy 経路で動く。
    if (auto* inst_data = const_cast<AnimationInstance*>(sys_.get_instance(inst))) {
        if (!inst_data->layers.empty()) {
            const auto* remap = get_bone_remap(d.clip, skel);
            if (remap && !remap->empty()) {
                inst_data->layers.back().channel_remap = remap;
            }
        }
    }

    Active a;
    a.m = m;
    a.skel = skel;
    a.layer = 0;
    a.local_time = 0.0f;
    a.prev_local_time = 0.0f;
    a.section_idx = -1;
    a.blend_in_initial = a.blend_in_remaining = params.blend_in_time;
    a.blend_out_initial = a.blend_out_remaining = 0.0f;
    a.params = params;

    if (!params.start_section.empty()) {
        for (std::size_t i = 0; i < d.sections.size(); ++i) {
            if (d.sections[i].name == params.start_section) {
                a.section_idx = static_cast<int>(i);
                a.local_time = d.sections[i].start;
                a.prev_local_time = a.local_time;
                break;
            }
        }
    }
    actives_[inst] = std::move(a);
}

void MontagePlayer::jump_to_section(AnimationStateHandle inst, const std::string& name) {
    auto it = actives_.find(inst);
    if (it == actives_.end()) return;
    const auto& d = montages_[it->second.m.id - 1];
    for (std::size_t i = 0; i < d.sections.size(); ++i) {
        if (d.sections[i].name == name) {
            it->second.section_idx = static_cast<int>(i);
            it->second.local_time  = d.sections[i].start;
            it->second.prev_local_time = it->second.local_time;
            return;
        }
    }
}

void MontagePlayer::set_next_section(AnimationStateHandle inst, const std::string& name) {
    auto it = actives_.find(inst);
    if (it == actives_.end()) return;
    it->second.next_section_override = name;
}

void MontagePlayer::stop(AnimationStateHandle inst) {
    auto it = actives_.find(inst);
    if (it == actives_.end()) return;
    sys_.stop(inst, montages_[it->second.m.id - 1].clip);
    if (it->second.params.on_finish) it->second.params.on_finish();
    actives_.erase(it);
}

bool MontagePlayer::is_playing(AnimationStateHandle inst) const {
    return actives_.find(inst) != actives_.end();
}

bool MontagePlayer::sample_curve(AnimationStateHandle inst, const std::string& name, float& out) const {
    auto it = actives_.find(inst);
    if (it == actives_.end()) return false;
    const auto& d = montages_[it->second.m.id - 1];
    for (const auto& c : d.curves) {
        if (c.name != name || c.times.empty()) continue;
        const float t = it->second.local_time;
        if (t <= c.times.front()) { out = c.values.front(); return true; }
        if (t >= c.times.back())  { out = c.values.back();  return true; }
        for (std::size_t i = 1; i < c.times.size(); ++i) {
            if (t < c.times[i]) {
                const float u = (t - c.times[i-1]) / (c.times[i] - c.times[i-1]);
                out = c.values[i-1] + (c.values[i] - c.values[i-1]) * u;
                return true;
            }
        }
    }
    return false;
}

void MontagePlayer::update(float dt) {
    if (dt <= 0.0f) return;
    std::vector<uint32_t> dead;
    for (auto& [inst_id, a] : actives_) {
        const auto& d = montages_[a.m.id - 1];

        a.prev_local_time = a.local_time;
        a.local_time += dt * a.params.speed;

        float weight = a.params.weight;
        if (a.blend_in_remaining > 0.0f) {
            a.blend_in_remaining -= dt;
            const float u = (a.blend_in_initial > 0.0f)
                              ? 1.0f - std::max(0.0f, a.blend_in_remaining) / a.blend_in_initial
                              : 1.0f;
            weight = a.params.weight * std::min(1.0f, u);
        }
        if (a.finishing) {
            a.blend_out_remaining -= dt;
            const float u = (a.blend_out_initial > 0.0f)
                              ? std::max(0.0f, a.blend_out_remaining) / a.blend_out_initial
                              : 0.0f;
            weight = a.params.weight * std::max(0.0f, u);
        }
        sys_.set_layer_weight(inst_id, 0, weight);

        const float lo = std::min(a.prev_local_time, a.local_time);
        const float hi = std::max(a.prev_local_time, a.local_time);
        for (const auto& n : d.notifies) {
            if (n.time > lo && n.time <= hi) {
                if (a.params.on_notify) a.params.on_notify(n);
            }
        }

        if (a.section_idx >= 0 && a.section_idx < static_cast<int>(d.sections.size())) {
            const auto& s = d.sections[a.section_idx];
            if (a.local_time >= s.end) {
                std::string nx = a.next_section_override.empty() ? s.next : a.next_section_override;
                a.next_section_override.clear();
                if (nx.empty()) {
                    a.finishing = true;
                    a.blend_out_initial = a.blend_out_remaining
                        = std::max(0.0f, a.params.blend_out_time);
                } else {
                    bool found = false;
                    for (std::size_t i = 0; i < d.sections.size(); ++i) {
                        if (d.sections[i].name == nx) {
                            a.section_idx = static_cast<int>(i);
                            a.local_time = d.sections[i].start;
                            a.prev_local_time = a.local_time;
                            found = true; break;
                        }
                    }
                    if (!found) a.finishing = true;
                }
            }
        } else {
            const AnimationClip* c = sys_.get_clip(d.clip);
            if (c && d.wrap == WrapMode::ONCE && a.local_time >= c->duration()) {
                a.finishing = true;
                if (a.blend_out_initial <= 0.0f) {
                    a.blend_out_initial = a.blend_out_remaining
                        = std::max(0.0f, a.params.blend_out_time);
                }
            }
        }

        if (a.finishing && a.blend_out_remaining <= 0.0f) {
            sys_.stop(inst_id, d.clip);
            if (a.params.on_finish) a.params.on_finish();
            dead.push_back(inst_id);
        }
    }
    for (uint32_t id : dead) actives_.erase(id);
}

} // namespace pictor
