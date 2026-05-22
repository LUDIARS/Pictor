#include "pictor/postprocess/postprocess_config_bridge.h"

#include "pictor/pipeline/pipeline_profile.h"

namespace pictor {

namespace {

/// Resolve a def's effect kind. JSON may carry an explicit `kind`; if it is
/// UNKNOWN we fall back to inferring it from `name` (the preset spelling).
PostProcessKind resolve_kind(const PostProcessDef& def) {
    if (def.kind != PostProcessKind::UNKNOWN) return def.kind;
    return post_process_kind_from_name(def.name);
}

} // namespace

PostProcessConfig build_post_process_config(const std::vector<PostProcessDef>& stack,
                                            const PostProcessConfig& base) {
    PostProcessConfig cfg = base;

    // The five effects a PostProcessDef can express are driven entirely by
    // the stack: start them disabled so an effect absent from the profile is
    // actually off. `hdr` / `gaussian_blur` have no PostProcessDef mapping —
    // they keep whatever `base` provided.
    cfg.bloom.enabled          = false;
    cfg.tone_mapping.enabled   = false;
    cfg.vignette.enabled       = false;
    cfg.color_grading.enabled  = false;
    cfg.depth_of_field.enabled = false;

    for (const auto& def : stack) {
        switch (resolve_kind(def)) {
            case PostProcessKind::BLOOM:
                cfg.bloom         = def.bloom;
                cfg.bloom.enabled = def.enabled;
                break;
            case PostProcessKind::TONE_MAPPING:
                cfg.tone_mapping         = def.tone_mapping;
                cfg.tone_mapping.enabled = def.enabled;
                break;
            case PostProcessKind::VIGNETTE:
                cfg.vignette         = def.vignette;
                cfg.vignette.enabled = def.enabled;
                break;
            case PostProcessKind::COLOR_GRADING:
                cfg.color_grading         = def.color_grading;
                cfg.color_grading.enabled = def.enabled;
                break;
            case PostProcessKind::DEPTH_OF_FIELD:
                cfg.depth_of_field         = def.depth_of_field;
                cfg.depth_of_field.enabled = def.enabled;
                break;
            case PostProcessKind::UNKNOWN:
                // SSAO / TAA / FXAA / VolumetricFog … — no host-driven
                // implementation in PostProcessPipeline. Ignored (phase 2).
                break;
        }
    }
    return cfg;
}

PostProcessConfig build_post_process_config(const PipelineProfileDef& profile,
                                            const PostProcessConfig& base) {
    return build_post_process_config(profile.post_process_stack, base);
}

} // namespace pictor
