#pragma once

#include "pictor/postprocess/postprocess_effect.h"

namespace pictor {

/// Depth of Field post-process effect.
///
/// Simulates camera bokeh blur based on depth:
///   1. Compute CoC (circle of confusion) per pixel from depth
///   2. Separate near-field and far-field
///   3. Apply disc blur with variable radius based on CoC
///   4. Composite near/far/sharp regions
///
/// Uses a physically-based thin-lens model where CoC = |f * (S - D)| / (D * (S - f))
/// simplified to linear ramps for real-time approximation.
class DepthOfFieldEffect : public PostProcessEffect {
public:
    DepthOfFieldEffect();
    explicit DepthOfFieldEffect(const DepthOfFieldConfig& config);
    ~DepthOfFieldEffect() override;

    const char* name() const override { return "DepthOfField"; }

    bool is_enabled() const override { return config_.enabled; }
    void set_enabled(bool enabled) override { config_.enabled = enabled; }

    void initialize(uint32_t width, uint32_t height) override;
    void resize(uint32_t width, uint32_t height) override;
    void shutdown() override;

    void execute(TextureHandle input_color,
                 TextureHandle input_depth,
                 TextureHandle output_color,
                 float delta_time) override;

    void set_config(const DepthOfFieldConfig& config) { config_ = config; }
    const DepthOfFieldConfig& config() const { return config_; }

private:
    DepthOfFieldConfig config_;
    uint32_t width_  = 0;
    uint32_t height_ = 0;
    bool     initialized_ = false;

    // Internal render targets
    TextureHandle coc_texture_       = INVALID_TEXTURE;
    TextureHandle near_field_        = INVALID_TEXTURE;
    TextureHandle far_field_         = INVALID_TEXTURE;
    TextureHandle near_field_blurred_ = INVALID_TEXTURE;
    TextureHandle far_field_blurred_  = INVALID_TEXTURE;

    void compute_coc(TextureHandle depth, TextureHandle coc_out);
    void separate_fields(TextureHandle color, TextureHandle coc,
                         TextureHandle near_out, TextureHandle far_out);
    void blur_field(TextureHandle input, TextureHandle output,
                    uint32_t width, uint32_t height);
    void composite_dof(TextureHandle sharp, TextureHandle near_blur,
                       TextureHandle far_blur, TextureHandle coc,
                       TextureHandle output);
};

} // namespace pictor
