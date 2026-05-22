#pragma once

/// ポストプロセスの設定構造体。 実 Vulkan の実行は `PostProcessPipeline`
/// (host-driven) が担う。 旧 `PostProcessEffect` 抽象クラスと per-effect
/// クラス群 (BloomEffect 等) は実装パイプラインへ統合され撤去済み。

#include <string>
#include <cstdint>

namespace pictor {

/// HDR rendering configuration.
/// Controls the floating-point color buffer used before tone mapping.
struct HDRConfig {
    bool     enabled         = true;
    float    exposure        = 1.0f;     ///< Exposure multiplier (EV)
    float    gamma           = 2.2f;     ///< Display gamma
    float    white_point     = 4.0f;     ///< Whitepoint for extended-Reinhard
    float    min_luminance   = 0.001f;   ///< Minimum scene luminance (auto-exposure)
    float    max_luminance   = 10.0f;    ///< Maximum scene luminance (auto-exposure)
    bool     auto_exposure   = false;    ///< Enable auto-exposure via luminance histogram
    float    adaptation_rate = 1.5f;     ///< Auto-exposure adaptation speed (seconds)
};

/// Tone-mapping operator selection.
enum class ToneMapOperator : uint8_t {
    ACES_FILMIC     = 0,  ///< Academy Color Encoding System filmic curve
    REINHARD        = 1,  ///< Simple Reinhard (luminance-based)
    REINHARD_EXT    = 2,  ///< Extended Reinhard with white point
    UNCHARTED2      = 3,  ///< Hable / Uncharted 2 filmic
    LINEAR_CLAMP    = 4,  ///< No tone mapping, just clamp
};

/// Bloom effect configuration.
struct BloomConfig {
    bool     enabled         = true;
    float    threshold       = 1.0f;     ///< Brightness threshold for bloom extraction
    float    soft_threshold  = 0.5f;     ///< Soft knee transition width
    float    intensity       = 0.8f;     ///< Bloom intensity multiplier
    float    radius          = 5.0f;     ///< Bloom blur radius (in pixels at half-res)
    uint32_t mip_levels      = 5;        ///< Number of downsampling steps (blur quality)
    float    scatter         = 0.7f;     ///< Mip-level scatter weight (progressive contribution)
};

/// Depth of Field configuration.
struct DepthOfFieldConfig {
    bool     enabled         = false;
    float    focus_distance  = 10.0f;    ///< Distance to the focal plane (world units)
    float    focus_range     = 5.0f;     ///< Sharp focus depth range
    float    bokeh_radius    = 4.0f;     ///< Maximum blur radius (pixels)
    float    near_start      = 0.0f;     ///< Near DoF start distance
    float    near_end        = 3.0f;     ///< Near DoF end distance (fully blurred)
    float    far_start       = 15.0f;    ///< Far DoF start distance
    float    far_end         = 50.0f;    ///< Far DoF end distance (fully blurred)
    uint32_t sample_count    = 16;       ///< Bokeh disc sample count
};

/// Gaussian blur configuration.
struct GaussianBlurConfig {
    bool     enabled         = false;
    float    sigma           = 2.0f;     ///< Standard deviation (kernel width)
    uint32_t kernel_size     = 9;        ///< Kernel tap count (odd, clamped 3..127)
    bool     separable       = true;     ///< Use separable 2-pass (H+V) blur
    float    intensity       = 1.0f;     ///< Output intensity multiplier
};

/// Tone-mapping configuration.
struct ToneMappingConfig {
    bool              enabled   = true;
    ToneMapOperator   op        = ToneMapOperator::ACES_FILMIC;
    float             exposure  = 1.0f;   ///< EV bias applied before tone mapping
    float             gamma     = 2.2f;   ///< Final gamma correction
    float             white_point = 4.0f; ///< Used with REINHARD_EXT
    float             saturation  = 1.0f; ///< Post-tonemap saturation (1.0 = unchanged)
};

/// Vignette configuration — darkens (or tints) screen edges.
struct VignetteConfig {
    bool   enabled   = true;
    float  intensity = 0.35f;          ///< 0 = none, 1 = strong edge darkening
    float  radius    = 0.75f;          ///< Normalized distance where falloff starts
    float  softness  = 0.45f;          ///< Falloff width (larger = smoother)
    float  color[3]  = {0.0f, 0.0f, 0.0f}; ///< Edge tint (usually black)
};

/// Color-grading configuration — applies a neutral LUT strip (PNG).
/// The strip is a `lut_size` tall by `lut_size*lut_size` wide RGBA8 image
/// (e.g. 16x256), the standard Unity / industry "neutral LUT" layout.
struct ColorGradingConfig {
    bool        enabled       = false;
    std::string lut_path;              ///< PNG LUT strip; empty disables the LUT
    float       lut_intensity = 1.0f;  ///< Blend 0..1 between original and graded
    uint32_t    lut_size      = 16;    ///< LUT cube edge (strip = size x size*size)
};

/// Aggregated post-process stack configuration.
/// Determines which effects are active and their parameters.
struct PostProcessConfig {
    HDRConfig            hdr;
    BloomConfig          bloom;
    DepthOfFieldConfig   depth_of_field;
    GaussianBlurConfig   gaussian_blur;
    ToneMappingConfig    tone_mapping;
    VignetteConfig       vignette;
    ColorGradingConfig   color_grading;
};

} // namespace pictor
