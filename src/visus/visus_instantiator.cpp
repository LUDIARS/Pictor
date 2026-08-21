#include "pictor/visus/visus_instantiator.h"

#include <cmath>
#include <limits>

namespace pictor {

namespace {

uint64_t number_or(const VisusMetadata& m, const char* key,
                   uint64_t def, uint64_t max) {
    const auto v = m.get_number(key);
    if (!v || !std::isfinite(*v) || *v < 0.0 ||
        *v > static_cast<double>(max) ||
        std::trunc(*v) != *v) return def;
    return static_cast<uint64_t>(*v);
}

} // namespace

ObjectDescriptor visus_base_descriptor(const VisusDesc& desc,
                                       const float4x4&  transform,
                                       const AABB&      bounds)
{
    ObjectDescriptor base{};
    base.transform = transform;
    base.bounds    = bounds;

    const uint16_t flags = static_cast<uint16_t>(
        number_or(desc.metadata, visus_keys::kRenderFlags, ObjectFlags::DYNAMIC,
                  std::numeric_limits<uint16_t>::max()));
    const uint16_t layer = static_cast<uint16_t>(
        number_or(desc.metadata, visus_keys::kRenderLayer, 0, 3));
    base.flags     = ObjectFlags::set_layer(flags, layer);
    base.lodLevel  = static_cast<uint8_t>(
        number_or(desc.metadata, visus_keys::kRenderLod, 0,
                  std::numeric_limits<uint8_t>::max()));
    // ShaderKey reserves the upper 32 bits for the custom-shader flag and
    // handle. Metadata may only supply the lower variant bits; otherwise an
    // untrusted Visus file could synthesize a resolved custom shader.
    base.shaderKey = number_or(desc.metadata, visus_keys::kShaderKeyOverride, 0,
                               std::numeric_limits<uint32_t>::max());
    return base;
}

} // namespace pictor
