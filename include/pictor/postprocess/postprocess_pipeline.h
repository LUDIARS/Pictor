#pragma once

#include "pictor/postprocess/postprocess_effect.h"
#include "pictor/postprocess/bloom_effect.h"
#include "pictor/postprocess/depth_of_field_effect.h"
#include "pictor/postprocess/tone_mapping_effect.h"
#include "pictor/postprocess/gaussian_blur_effect.h"
#include <memory>
#include <vector>

namespace pictor {

/// Post-process pipeline: manages and executes a stack of effects.
///
/// Default effect execution order (matching typical HDR pipeline):
///   1. Bloom          — extract & scatter bright areas
///   2. Depth of Field — camera bokeh simulation
///   3. Gaussian Blur  — general-purpose full-screen blur
///   4. Tone Mapping   — HDR → LDR conversion + gamma (always last)
///
/// The pipeline manages ping-pong render targets internally so
/// consecutive effects chain without explicit user wiring.
class PostProcessPipeline {
public:
    PostProcessPipeline();
    ~PostProcessPipeline();

    PostProcessPipeline(const PostProcessPipeline&) = delete;
    PostProcessPipeline& operator=(const PostProcessPipeline&) = delete;

    /// Initialize pipeline with screen dimensions and full config
    void initialize(uint32_t width, uint32_t height, const PostProcessConfig& config);

    /// Resize all internal resources on viewport change
    void resize(uint32_t width, uint32_t height);

    /// Release all resources
    void shutdown();

    bool is_initialized() const { return initialized_; }

    /// Execute the full post-process stack.
    ///
    /// @param scene_color  HDR color output from the scene render pass
    /// @param scene_depth  Scene depth buffer (linear depth)
    /// @param final_output Destination render target (swapchain or LDR buffer)
    /// @param delta_time   Frame delta time
    void execute(TextureHandle scene_color,
                 TextureHandle scene_depth,
                 TextureHandle final_output,
                 float delta_time);

    /// Apply a full configuration update
    void set_config(const PostProcessConfig& config);
    const PostProcessConfig& config() const { return config_; }

    // --- Per-effect access ---

    BloomEffect&          bloom()          { return *bloom_; }
    DepthOfFieldEffect&   depth_of_field() { return *dof_; }
    GaussianBlurEffect&   gaussian_blur()  { return *blur_; }
    ToneMappingEffect&    tone_mapping()   { return *tonemap_; }

    const BloomEffect&        bloom()          const { return *bloom_; }
    const DepthOfFieldEffect& depth_of_field() const { return *dof_; }
    const GaussianBlurEffect& gaussian_blur()  const { return *blur_; }
    const ToneMappingEffect&  tone_mapping()   const { return *tonemap_; }

    /// Get the ordered list of all effects (for iteration / UI)
    const std::vector<PostProcessEffect*>& effect_stack() const { return effect_stack_; }

    // --- HDR Config ---

    void set_hdr_config(const HDRConfig& hdr) { config_.hdr = hdr; }
    const HDRConfig& hdr_config() const { return config_.hdr; }

    /// Get count of currently enabled effects
    uint32_t enabled_effect_count() const;

private:
    PostProcessConfig config_;

    std::unique_ptr<BloomEffect>        bloom_;
    std::unique_ptr<DepthOfFieldEffect> dof_;
    std::unique_ptr<GaussianBlurEffect> blur_;
    std::unique_ptr<ToneMappingEffect>  tonemap_;

    std::vector<PostProcessEffect*> effect_stack_;

    // Ping-pong render targets for chaining effects
    TextureHandle ping_target_ = INVALID_TEXTURE;
    TextureHandle pong_target_ = INVALID_TEXTURE;

    uint32_t width_  = 0;
    uint32_t height_ = 0;
    bool     initialized_ = false;

    void build_effect_stack();
};

} // namespace pictor
