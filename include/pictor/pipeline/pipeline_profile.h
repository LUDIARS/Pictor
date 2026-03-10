#pragma once

#include "pictor/core/types.h"
#include "pictor/memory/memory_subsystem.h"
#include "pictor/update/update_scheduler.h"
#include "pictor/gpu/gpu_driven_pipeline.h"
#include <string>
#include <vector>

namespace pictor {

/// Shadow configuration (§8.2)
struct ShadowConfig {
    uint32_t cascade_count   = 3;
    uint32_t resolution      = 2048;
    bool     pcf_filtering   = true;
};

/// Post-process effect definition (§8.2)
struct PostProcessDef {
    std::string name;
    bool        enabled = true;
};

/// Render pass definition (§8.3)
struct RenderPassDef {
    std::string             pass_name;
    PassType                pass_type        = PassType::OPAQUE;
    ShaderHandle            shader_override  = INVALID_MESH;
    std::vector<std::string> render_targets;
    std::vector<std::string> input_textures;
    SortMode                sort_mode        = SortMode::FRONT_TO_BACK;
    uint16_t                filter_mask      = 0xFFFF;
    bool                    gpu_driven_pass  = false;
    std::vector<std::string> required_streams; // prefetch hints
};

/// Profiler configuration (§8.2)
struct ProfilerConfig {
    bool        enabled       = true;
    OverlayMode overlay_mode  = OverlayMode::STANDARD;
    uint32_t    max_queries   = 64;
};

/// Pipeline profile definition (§8.2).
/// Corresponds to Lite/Standard/Ultra presets or custom profiles.
struct PipelineProfileDef {
    std::string                profile_name;
    RenderingPath              rendering_path       = RenderingPath::FORWARD_PLUS;
    std::vector<RenderPassDef> render_passes;
    ShadowConfig               shadow_config;
    std::vector<PostProcessDef> post_process_stack;
    bool                       gpu_driven_enabled   = true;
    bool                       compute_update_enabled = true;
    GPUDrivenConfig            gpu_driven_config;
    MemoryConfig               memory_config;
    UpdateConfig               update_config;
    ProfilerConfig             profiler_config;
    uint32_t                   max_lights           = 256;
    uint8_t                    msaa_samples         = 0;
};

/// Pipeline profile manager (§8.4, §12).
/// Provides built-in presets and supports custom profile registration.
class PipelineProfileManager {
public:
    PipelineProfileManager();
    ~PipelineProfileManager();

    /// Register built-in profiles (Lite, Standard, Ultra)
    void register_defaults();

    /// Register a custom profile (§12)
    void register_profile(const PipelineProfileDef& def);

    /// Set active profile by name (§8.4)
    bool set_profile(const std::string& name);

    /// Get current profile
    const PipelineProfileDef& current_profile() const { return *current_; }
    const std::string& current_profile_name() const { return current_->profile_name; }

    /// Get profile by name
    const PipelineProfileDef* get_profile(const std::string& name) const;

    /// List all registered profile names
    std::vector<std::string> profile_names() const;

    /// Create built-in profiles (§8.1)
    static PipelineProfileDef create_lite_profile();
    static PipelineProfileDef create_standard_profile();
    static PipelineProfileDef create_ultra_profile();

private:
    std::vector<PipelineProfileDef> profiles_;
    const PipelineProfileDef*       current_ = nullptr;
};

} // namespace pictor
