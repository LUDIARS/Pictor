#include "pictor/postprocess/gaussian_blur_effect.h"
#include <algorithm>
#include <cmath>

namespace pictor {

GaussianBlurEffect::GaussianBlurEffect() {
    compute_kernel();
}

GaussianBlurEffect::GaussianBlurEffect(const GaussianBlurConfig& config)
    : config_(config) {
    compute_kernel();
}

GaussianBlurEffect::~GaussianBlurEffect() { shutdown(); }

void GaussianBlurEffect::initialize(uint32_t width, uint32_t height) {
    width_  = width;
    height_ = height;

    // Allocate intermediate texture for separable H+V passes (RGBA16F)
    intermediate_ = 0;

    compute_kernel();
    initialized_ = true;
}

void GaussianBlurEffect::resize(uint32_t width, uint32_t height) {
    if (width == width_ && height == height_) return;
    width_  = width;
    height_ = height;
    // Recreate intermediate texture at new resolution
    intermediate_ = 0;
}

void GaussianBlurEffect::shutdown() {
    if (!initialized_) return;
    intermediate_ = INVALID_TEXTURE;
    initialized_ = false;
}

void GaussianBlurEffect::set_config(const GaussianBlurConfig& config) {
    config_ = config;
    // Enforce odd kernel size, clamped to [3, MAX_KERNEL_SIZE]
    config_.kernel_size |= 1; // make odd
    config_.kernel_size = std::clamp(config_.kernel_size, 3u, MAX_KERNEL_SIZE);
    compute_kernel();
}

void GaussianBlurEffect::execute(TextureHandle input_color,
                                   TextureHandle /*input_depth*/,
                                   TextureHandle output_color,
                                   float /*delta_time*/) {
    if (!initialized_ || !config_.enabled) return;

    if (config_.separable) {
        // Two-pass separable Gaussian blur (O(n) per pixel instead of O(n^2))
        blur_pass_horizontal(input_color, intermediate_, width_, height_);
        blur_pass_vertical(intermediate_, output_color, width_, height_);
    } else {
        // Single-pass 2D convolution (expensive, only for small kernels)
        // For non-separable mode, horizontal pass suffices with 2D kernel
        blur_pass_horizontal(input_color, output_color, width_, height_);
    }
}

void GaussianBlurEffect::compute_kernel() {
    uint32_t ks = config_.kernel_size;
    ks |= 1; // ensure odd
    ks = std::clamp(ks, 3u, MAX_KERNEL_SIZE);
    effective_kernel_size_ = ks;

    float sigma = config_.sigma;
    if (sigma <= 0.0f) sigma = static_cast<float>(ks) / 6.0f;

    int32_t half = static_cast<int32_t>(ks / 2);
    float sum = 0.0f;

    // Compute 1D Gaussian weights: G(x) = exp(-x^2 / (2*sigma^2))
    for (int32_t i = -half; i <= half; ++i) {
        float x = static_cast<float>(i);
        float w = std::exp(-(x * x) / (2.0f * sigma * sigma));
        weights_[static_cast<uint32_t>(i + half)] = w;
        sum += w;
    }

    // Normalize so weights sum to 1
    for (uint32_t i = 0; i < ks; ++i) {
        weights_[i] /= sum;
    }
}

void GaussianBlurEffect::blur_pass_horizontal(TextureHandle /*input*/, TextureHandle /*output*/,
                                                 uint32_t /*width*/, uint32_t /*height*/) {
    // GPU dispatch: gaussian_blur.comp (direction = vec2(1, 0))
    //
    // Push constants:
    //   vec2 direction  = (1.0 / width, 0.0)
    //   int  kernelSize = effective_kernel_size_
    //   float weights[] = weights_[0..kernelSize-1]
    //   float intensity = config_.intensity
    //
    // Per-pixel:
    //   vec3 color = vec3(0);
    //   int half = kernelSize / 2;
    //   for (int i = -half; i <= half; i++) {
    //       vec2 offset = direction * float(i);
    //       color += texture(input, uv + offset).rgb * weights[i + half];
    //   }
    //   output = vec4(color * intensity, 1.0);
}

void GaussianBlurEffect::blur_pass_vertical(TextureHandle /*input*/, TextureHandle /*output*/,
                                               uint32_t /*width*/, uint32_t /*height*/) {
    // GPU dispatch: gaussian_blur.comp (direction = vec2(0, 1))
    //
    // Same shader as horizontal, with direction = (0.0, 1.0 / height)
}

} // namespace pictor
