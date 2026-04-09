#pragma once

#include "pictor/postprocess/postprocess_effect.h"

namespace pictor {

/// Gaussian Blur post-process effect.
///
/// Efficient separable Gaussian blur:
///   1. Horizontal pass: convolve rows with 1D Gaussian kernel
///   2. Vertical pass: convolve columns with 1D Gaussian kernel
///
/// Kernel weights are precomputed from the specified sigma.
/// For large kernels (>15 taps), uses linear sampling optimization
/// to halve the number of texture fetches.
class GaussianBlurEffect : public PostProcessEffect {
public:
    GaussianBlurEffect();
    explicit GaussianBlurEffect(const GaussianBlurConfig& config);
    ~GaussianBlurEffect() override;

    const char* name() const override { return "GaussianBlur"; }

    bool is_enabled() const override { return config_.enabled; }
    void set_enabled(bool enabled) override { config_.enabled = enabled; }

    void initialize(uint32_t width, uint32_t height) override;
    void resize(uint32_t width, uint32_t height) override;
    void shutdown() override;

    void execute(TextureHandle input_color,
                 TextureHandle input_depth,
                 TextureHandle output_color,
                 float delta_time) override;

    void set_config(const GaussianBlurConfig& config);
    const GaussianBlurConfig& config() const { return config_; }

private:
    GaussianBlurConfig config_;
    uint32_t width_  = 0;
    uint32_t height_ = 0;
    bool     initialized_ = false;

    // Intermediate texture for separable blur
    TextureHandle intermediate_ = INVALID_TEXTURE;

    // Precomputed kernel weights
    static constexpr uint32_t MAX_KERNEL_SIZE = 127;
    float   weights_[MAX_KERNEL_SIZE] = {};
    uint32_t effective_kernel_size_ = 0;

    void compute_kernel();
    void blur_pass_horizontal(TextureHandle input, TextureHandle output,
                              uint32_t width, uint32_t height);
    void blur_pass_vertical(TextureHandle input, TextureHandle output,
                            uint32_t width, uint32_t height);
};

} // namespace pictor
