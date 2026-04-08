#include "pictor/postprocess/bloom_effect.h"
#include <algorithm>
#include <cmath>

namespace pictor {

BloomEffect::BloomEffect() = default;
BloomEffect::BloomEffect(const BloomConfig& config) : config_(config) {}
BloomEffect::~BloomEffect() { shutdown(); }

void BloomEffect::initialize(uint32_t width, uint32_t height) {
    width_  = width;
    height_ = height;

    // Calculate active mip count based on config and resolution
    uint32_t min_dim = std::min(width, height);
    active_mip_count_ = 0;
    while (min_dim > 1 && active_mip_count_ < config_.mip_levels &&
           active_mip_count_ < MAX_MIP_LEVELS) {
        min_dim >>= 1;
        ++active_mip_count_;
    }
    active_mip_count_ = std::max(active_mip_count_, 1u);

    // Allocate mip chain textures (RGBA16F for HDR precision)
    // In a Vulkan implementation these would be VkImage + VkImageView per mip
    uint32_t mip_w = width >> 1;
    uint32_t mip_h = height >> 1;
    for (uint32_t i = 0; i < active_mip_count_; ++i) {
        mip_chain_[i] = i; // Placeholder handle assignment
        mip_w = std::max(mip_w >> 1, 1u);
        mip_h = std::max(mip_h >> 1, 1u);
    }

    initialized_ = true;
}

void BloomEffect::resize(uint32_t width, uint32_t height) {
    if (width == width_ && height == height_) return;
    shutdown();
    initialize(width, height);
}

void BloomEffect::shutdown() {
    if (!initialized_) return;

    for (uint32_t i = 0; i < active_mip_count_; ++i) {
        mip_chain_[i] = INVALID_TEXTURE;
    }
    active_mip_count_ = 0;
    initialized_ = false;
}

void BloomEffect::execute(TextureHandle input_color,
                           TextureHandle /*input_depth*/,
                           TextureHandle output_color,
                           float /*delta_time*/) {
    if (!initialized_ || !config_.enabled || active_mip_count_ == 0) return;

    // Step 1: Extract bright pixels above threshold
    // Dispatch compute shader: bloom_extract.comp
    //   - Reads: input_color (HDR scene)
    //   - Writes: mip_chain_[0] (half-res bright regions)
    //   - Uniforms: threshold, soft_threshold
    extract_bright_pixels(input_color, mip_chain_[0]);

    // Step 2: Progressive downsample through mip chain
    // Uses 13-tap filter for anti-aliasing (Karis average on first mip)
    uint32_t mip_w = width_ >> 1;
    uint32_t mip_h = height_ >> 1;
    for (uint32_t i = 1; i < active_mip_count_; ++i) {
        mip_w = std::max(mip_w >> 1, 1u);
        mip_h = std::max(mip_h >> 1, 1u);
        downsample_pass(mip_chain_[i - 1], mip_chain_[i], mip_w, mip_h);
    }

    // Step 3: Progressive upsample with accumulation
    // Uses 9-tap tent filter for smooth upsampling
    for (int32_t i = static_cast<int32_t>(active_mip_count_) - 2; i >= 0; --i) {
        uint32_t up_w = width_ >> (static_cast<uint32_t>(i) + 1);
        uint32_t up_h = height_ >> (static_cast<uint32_t>(i) + 1);
        up_w = std::max(up_w, 1u);
        up_h = std::max(up_h, 1u);

        float mip_scatter = config_.scatter;
        upsample_pass(mip_chain_[i + 1], mip_chain_[i], up_w, up_h, mip_scatter);
    }

    // Step 4: Composite bloom with original scene color
    composite(mip_chain_[0], input_color, output_color, config_.intensity);
}

void BloomEffect::extract_bright_pixels(TextureHandle /*input*/,
                                         TextureHandle /*output*/) {
    // GPU dispatch: bloom_extract.comp
    // threshold = config_.threshold
    // soft_knee = config_.soft_threshold
    //
    // Per-pixel: luminance = dot(color.rgb, vec3(0.2126, 0.7152, 0.0722))
    //            contribution = max(0, luminance - threshold + soft_knee)
    //            contribution = clamp(contribution / (2.0 * soft_knee + 0.00001), 0, 1)
    //            output = color.rgb * contribution
}

void BloomEffect::downsample_pass(TextureHandle /*input*/, TextureHandle /*output*/,
                                    uint32_t /*out_width*/, uint32_t /*out_height*/) {
    // GPU dispatch: bloom_downsample.comp
    // 13-tap downsampling filter (box filter with better quality):
    //   A . B . C
    //   . D . E .
    //   F . G . H
    //   . I . J .
    //   K . L . M
    //
    // output = (D+E+I+J)*0.5*0.25 + (A+B+F+G)*0.125*0.25 + ...
}

void BloomEffect::upsample_pass(TextureHandle /*input*/, TextureHandle /*output*/,
                                  uint32_t /*out_width*/, uint32_t /*out_height*/,
                                  float /*scatter*/) {
    // GPU dispatch: bloom_upsample.comp
    // 9-tap tent filter (3x3 bilinear):
    //   1 2 1
    //   2 4 2
    //   1 2 1  (all / 16)
    //
    // output = existing * (1 - scatter) + upsampled * scatter
}

void BloomEffect::composite(TextureHandle /*bloom_tex*/, TextureHandle /*scene_color*/,
                              TextureHandle /*output*/, float /*intensity*/) {
    // GPU dispatch: bloom_composite.comp (or fullscreen quad fragment shader)
    // output = scene_color + bloom * intensity
}

} // namespace pictor
