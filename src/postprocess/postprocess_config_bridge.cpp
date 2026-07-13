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

    // The effects a PostProcessDef can express are driven entirely by
    // the stack: start them disabled so an effect absent from the profile is
    // actually off. `hdr` / `gaussian_blur` have no PostProcessDef mapping —
    // they keep whatever `base` provided.
    cfg.bloom.enabled                = false;
    cfg.tone_mapping.enabled         = false;
    cfg.vignette.enabled             = false;
    cfg.color_grading.enabled        = false;
    cfg.depth_of_field.enabled       = false;
    cfg.ssao.enabled                 = false;
    cfg.motion_blur.enabled          = false;
    cfg.fxaa.enabled                 = false;
    cfg.chromatic_aberration.enabled = false;
    cfg.film_grain.enabled           = false;
    cfg.taa.enabled                  = false;
    cfg.ssr.enabled                  = false;

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
            case PostProcessKind::SSAO:
                cfg.ssao         = def.ssao;
                cfg.ssao.enabled = def.enabled;
                break;
            case PostProcessKind::MOTION_BLUR:
                // 再投影行列はランタイムデータ — プロファイルからは
                // パラメータのみ写し、 行列はホスト更新を待つ
                // (matrix_valid は def 側の値に依らず false 起点)。
                cfg.motion_blur              = def.motion_blur;
                cfg.motion_blur.enabled      = def.enabled;
                cfg.motion_blur.matrix_valid = false;
                break;
            case PostProcessKind::FXAA:
                cfg.fxaa         = def.fxaa;
                cfg.fxaa.enabled = def.enabled;
                break;
            case PostProcessKind::CHROMATIC_ABERRATION:
                cfg.chromatic_aberration         = def.chromatic_aberration;
                cfg.chromatic_aberration.enabled = def.enabled;
                break;
            case PostProcessKind::FILM_GRAIN:
                cfg.film_grain         = def.film_grain;
                cfg.film_grain.enabled = def.enabled;
                break;
            case PostProcessKind::TAA:
                // 再投影行列 / ジッタ / history はランタイムデータ —
                // プロファイルからはパラメータのみ写し、 valid 類は
                // ホスト更新を待つ false 起点。
                cfg.taa               = def.taa;
                cfg.taa.enabled       = def.enabled;
                cfg.taa.matrix_valid  = false;
                cfg.taa.history_valid = false;
                break;
            case PostProcessKind::SSR:
                // proj_xx / proj_yy / near / far はカメラ由来のランタイム
                // データ — ホストが毎フレーム上書きする前提で写すだけ。
                cfg.ssr         = def.ssr;
                cfg.ssr.enabled = def.enabled;
                break;
            case PostProcessKind::UNKNOWN:
                // VolumetricFog / LensFlare … — no host-driven
                // implementation in PostProcessPipeline yet. Ignored.
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
