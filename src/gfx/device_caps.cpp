#include "pictor/gfx/device_caps.h"

namespace pictor::gfx {

const char* capability_tier_name(CapabilityTier tier) noexcept {
    switch (tier) {
        case CapabilityTier::A_Raster:  return "A_Raster";
        case CapabilityTier::B_Compute: return "B_Compute";
        case CapabilityTier::C_Full:    return "C_Full";
    }
    return "unknown";
}

const char* feature_name(Feature feature) noexcept {
    switch (feature) {
        case Feature::Compute:                 return "Compute";
        case Feature::StorageBufferVertex:     return "StorageBufferVertex";
        case Feature::StorageBufferFragment:   return "StorageBufferFragment";
        case Feature::StorageTexture:          return "StorageTexture";
        case Feature::IndirectDraw:            return "IndirectDraw";
        case Feature::IndirectDrawCount:       return "IndirectDrawCount";
        case Feature::MultiDrawIndirect:       return "MultiDrawIndirect";
        case Feature::FirstInstance:           return "FirstInstance";
        case Feature::TimestampQuery:          return "TimestampQuery";
        case Feature::TimestampInsidePass:     return "TimestampInsidePass";
        case Feature::OcclusionQuery:          return "OcclusionQuery";
        case Feature::Bindless:                return "Bindless";
        case Feature::Msaa4x:                  return "Msaa4x";
        case Feature::Msaa8x:                  return "Msaa8x";
        case Feature::ExplicitRenderPassMerge: return "ExplicitRenderPassMerge";
        case Feature::FloatRenderTarget:       return "FloatRenderTarget";
        case Feature::TextureCompressionBc:    return "TextureCompressionBc";
        case Feature::TextureCompressionEtc2:  return "TextureCompressionEtc2";
        case Feature::TextureCompressionAstc:  return "TextureCompressionAstc";
        case Feature::DepthClamp:              return "DepthClamp";
        case Feature::Count_:                  break;
    }
    return "unknown";
}

} // namespace pictor::gfx
