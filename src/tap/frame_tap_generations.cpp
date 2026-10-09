#include "pictor/tap/frame_tap_generations.h"

namespace pictor {

bool FrameTapGenerations::observe(FrameTapInstanceSpace space, uint32_t instance,
                                  uint64_t appearance, uint32_t& generation) {
    if (instance >= kMaxTrackedInstances) return false;
    std::vector<Entry>& table = (space == FrameTapInstanceSpace::UI) ? ui_ : scene_;
    if (instance >= table.size()) table.resize(static_cast<size_t>(instance) + 1);

    Entry& entry = table[instance];
    if (!entry.seen) {
        entry.seen       = 1;
        entry.appearance = appearance;
    } else if (entry.appearance != appearance) {
        ++entry.generation;
        entry.appearance = appearance;
    }
    generation = entry.generation;
    return true;
}

void FrameTapGenerations::reset() {
    scene_.clear();
    ui_.clear();
}

} // namespace pictor
