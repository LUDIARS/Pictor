#include "pictor/visus/visus_registry.h"

namespace pictor {

VisusHandle VisusRegistry::register_visus(VisusDesc desc) {
    const VisusHandle h = next_handle_++;
    visus_.push_back(std::move(desc));
    return h;
}

const VisusDesc* VisusRegistry::get(VisusHandle handle) const {
    if (handle >= visus_.size()) return nullptr;
    return &visus_[handle];
}

} // namespace pictor
