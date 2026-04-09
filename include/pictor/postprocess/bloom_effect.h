#pragma once

#include "pictor/postprocess/postprocess_effect.h"

namespace pictor {

/// Bloom post-process effect.
///
/// Multi-pass bloom using progressive downsampling + upsampling:
///   1. Brightness extraction (threshold filter)
///   2. Progressive downsample with 13-tap filter (mip chain)
///   3. Progressive upsample with 9-tap tent filter
///   4. Additive composite with original HDR color
///
/// This is a physically-motivated approximation of light scattering
/// on the lens/sensor, following the Karis (2014) approach used in UE4.
class BloomEffect : public PostProcessEffect {
public:
    BloomEffect();
    explicit BloomEffect(const BloomConfig& config);
    ~BloomEffect() override;

    const char* name() const override { return "Bloom"; }

    bool is_enabled() const override { return config_.enabled; }
    void set_enabled(bool enabled) override { config_.enabled = enabled; }

    void initialize(uint32_t width, uint32_t height) override;
    void resize(uint32_t width, uint32_t height) override;
    void shutdown() override;

    void execute(TextureHandle input_color,
                 TextureHandle input_depth,
                 TextureHandle output_color,
                 float delta_time) override;

    /// Update configuration at runtime
    void set_config(const BloomConfig& config) { config_ = config; }
    const BloomConfig& config() const { return config_; }

private:
    BloomConfig config_;
    uint32_t    width_  = 0;
    uint32_t    height_ = 0;
    bool        initialized_ = false;

    // Internal mip chain textures (would be actual GPU textures in production)
    static constexpr uint32_t MAX_MIP_LEVELS = 8;
    TextureHandle mip_chain_[MAX_MIP_LEVELS] = {};
    uint32_t      active_mip_count_ = 0;

    void extract_bright_pixels(TextureHandle input, TextureHandle output);
    void downsample_pass(TextureHandle input, TextureHandle output,
                         uint32_t out_width, uint32_t out_height);
    void upsample_pass(TextureHandle input, TextureHandle output,
                       uint32_t out_width, uint32_t out_height, float scatter);
    void composite(TextureHandle bloom_tex, TextureHandle scene_color,
                   TextureHandle output, float intensity);
};

} // namespace pictor
