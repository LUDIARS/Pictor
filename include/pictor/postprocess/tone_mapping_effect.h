#pragma once

#include "pictor/postprocess/postprocess_effect.h"

namespace pictor {

/// Tone Mapping post-process effect.
///
/// Maps HDR color values to displayable LDR range. Supports multiple
/// tone-mapping operators:
///   - ACES Filmic: Industry-standard cinematic curve
///   - Reinhard: Simple luminance-preserving operator
///   - Reinhard Extended: With configurable white point
///   - Uncharted 2: Hable's filmic curve with shoulder/toe
///   - Linear clamp: Pass-through (debug/reference)
///
/// Also applies exposure adjustment and final gamma correction.
class ToneMappingEffect : public PostProcessEffect {
public:
    ToneMappingEffect();
    explicit ToneMappingEffect(const ToneMappingConfig& config);
    ~ToneMappingEffect() override;

    const char* name() const override { return "ToneMapping"; }

    bool is_enabled() const override { return config_.enabled; }
    void set_enabled(bool enabled) override { config_.enabled = enabled; }

    void initialize(uint32_t width, uint32_t height) override;
    void resize(uint32_t width, uint32_t height) override;
    void shutdown() override;

    void execute(TextureHandle input_color,
                 TextureHandle input_depth,
                 TextureHandle output_color,
                 float delta_time) override;

    void set_config(const ToneMappingConfig& config) { config_ = config; }
    const ToneMappingConfig& config() const { return config_; }

    void set_operator(ToneMapOperator op) { config_.op = op; }
    void set_exposure(float exposure) { config_.exposure = exposure; }

private:
    ToneMappingConfig config_;
    uint32_t width_  = 0;
    uint32_t height_ = 0;
    bool     initialized_ = false;
};

} // namespace pictor
