#pragma once

#include "pictor/core/types.h"
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

/// Aggregated post-process stack configuration.
/// Determines which effects are active and their parameters.
struct PostProcessConfig {
    HDRConfig            hdr;
    BloomConfig          bloom;
    DepthOfFieldConfig   depth_of_field;
    GaussianBlurConfig   gaussian_blur;
    ToneMappingConfig    tone_mapping;
};

/// Abstract base class for a single post-process effect.
///
/// Each effect reads from an input framebuffer (HDR color + depth)
/// and writes to an output framebuffer. Effects are chained by the
/// PostProcessPipeline in stack order.
class PostProcessEffect {
public:
    virtual ~PostProcessEffect() = default;

    /// Human-readable name (e.g. "Bloom", "DoF")
    virtual const char* name() const = 0;

    /// Whether this effect is currently active
    virtual bool is_enabled() const = 0;
    virtual void set_enabled(bool enabled) = 0;

    /// Initialize GPU resources (called once at pipeline setup)
    virtual void initialize(uint32_t width, uint32_t height) = 0;

    /// Resize internal resources on viewport change
    virtual void resize(uint32_t width, uint32_t height) = 0;

    /// Release GPU resources
    virtual void shutdown() = 0;

    /// Execute the effect.
    ///
    /// @param input_color   Handle to the HDR color texture from the previous stage
    /// @param input_depth   Handle to the depth buffer (linear depth)
    /// @param output_color  Handle to the output render target
    /// @param delta_time    Frame delta time (for temporal effects)
    virtual void execute(TextureHandle input_color,
                         TextureHandle input_depth,
                         TextureHandle output_color,
                         float delta_time) = 0;
};

} // namespace pictor
