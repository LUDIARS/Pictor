#include "pictor/postprocess/postprocess_pipeline.h"
#include <algorithm>

namespace pictor {

PostProcessPipeline::PostProcessPipeline()
    : bloom_(std::make_unique<BloomEffect>())
    , dof_(std::make_unique<DepthOfFieldEffect>())
    , blur_(std::make_unique<GaussianBlurEffect>())
    , tonemap_(std::make_unique<ToneMappingEffect>()) {
}

PostProcessPipeline::~PostProcessPipeline() {
    shutdown();
}

void PostProcessPipeline::initialize(uint32_t width, uint32_t height,
                                       const PostProcessConfig& config) {
    width_  = width;
    height_ = height;
    config_ = config;

    // Configure each effect
    bloom_->set_config(config.bloom);
    dof_->set_config(config.depth_of_field);
    blur_->set_config(config.gaussian_blur);
    tonemap_->set_config(config.tone_mapping);

    // Initialize each effect
    bloom_->initialize(width, height);
    dof_->initialize(width, height);
    blur_->initialize(width, height);
    tonemap_->initialize(width, height);

    // Allocate ping-pong render targets (RGBA16F for HDR intermediate results)
    ping_target_ = 0;
    pong_target_ = 1;

    build_effect_stack();
    initialized_ = true;
}

void PostProcessPipeline::resize(uint32_t width, uint32_t height) {
    if (width == width_ && height == height_) return;
    width_  = width;
    height_ = height;

    bloom_->resize(width, height);
    dof_->resize(width, height);
    blur_->resize(width, height);
    tonemap_->resize(width, height);

    // Recreate ping-pong targets at new resolution
    ping_target_ = 0;
    pong_target_ = 1;
}

void PostProcessPipeline::shutdown() {
    if (!initialized_) return;

    bloom_->shutdown();
    dof_->shutdown();
    blur_->shutdown();
    tonemap_->shutdown();

    ping_target_ = INVALID_TEXTURE;
    pong_target_ = INVALID_TEXTURE;

    effect_stack_.clear();
    initialized_ = false;
}

void PostProcessPipeline::execute(TextureHandle scene_color,
                                    TextureHandle scene_depth,
                                    TextureHandle final_output,
                                    float delta_time) {
    if (!initialized_) return;

    // Count how many effects are enabled
    build_effect_stack();

    uint32_t active_count = 0;
    for (auto* effect : effect_stack_) {
        if (effect->is_enabled()) ++active_count;
    }

    if (active_count == 0) {
        // No effects enabled — blit scene_color directly to output
        // In production: vkCmdCopyImage or compute shader copy
        (void)scene_color;
        (void)final_output;
        return;
    }

    // Chain effects via ping-pong targets
    // First effect reads from scene_color, last writes to final_output
    TextureHandle current_input = scene_color;
    bool use_ping = true;
    uint32_t effects_executed = 0;

    for (auto* effect : effect_stack_) {
        if (!effect->is_enabled()) continue;

        ++effects_executed;
        bool is_last = (effects_executed == active_count);

        TextureHandle current_output = is_last ? final_output
                                               : (use_ping ? ping_target_ : pong_target_);

        effect->execute(current_input, scene_depth, current_output, delta_time);

        current_input = current_output;
        if (!is_last) use_ping = !use_ping;
    }
}

void PostProcessPipeline::set_config(const PostProcessConfig& config) {
    config_ = config;
    bloom_->set_config(config.bloom);
    dof_->set_config(config.depth_of_field);
    blur_->set_config(config.gaussian_blur);
    tonemap_->set_config(config.tone_mapping);
    build_effect_stack();
}

uint32_t PostProcessPipeline::enabled_effect_count() const {
    uint32_t count = 0;
    for (auto* effect : effect_stack_) {
        if (effect->is_enabled()) ++count;
    }
    return count;
}

void PostProcessPipeline::build_effect_stack() {
    // Fixed execution order:
    //   1. Bloom (operates on HDR values, must be before tone mapping)
    //   2. DoF (operates on HDR values with depth)
    //   3. Gaussian Blur (general-purpose blur)
    //   4. Tone Mapping (HDR→LDR, always last)
    effect_stack_.clear();
    effect_stack_.push_back(bloom_.get());
    effect_stack_.push_back(dof_.get());
    effect_stack_.push_back(blur_.get());
    effect_stack_.push_back(tonemap_.get());
}

} // namespace pictor
