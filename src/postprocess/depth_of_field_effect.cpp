#include "pictor/postprocess/depth_of_field_effect.h"
#include <algorithm>
#include <cmath>

namespace pictor {

DepthOfFieldEffect::DepthOfFieldEffect() = default;
DepthOfFieldEffect::DepthOfFieldEffect(const DepthOfFieldConfig& config) : config_(config) {}
DepthOfFieldEffect::~DepthOfFieldEffect() { shutdown(); }

void DepthOfFieldEffect::initialize(uint32_t width, uint32_t height) {
    width_  = width;
    height_ = height;

    // Allocate internal render targets
    // CoC is stored as R16F (signed: negative=near, positive=far)
    coc_texture_ = 0;
    // Near and far fields at half resolution for performance
    near_field_         = 1;
    far_field_          = 2;
    near_field_blurred_ = 3;
    far_field_blurred_  = 4;

    initialized_ = true;
}

void DepthOfFieldEffect::resize(uint32_t width, uint32_t height) {
    if (width == width_ && height == height_) return;
    shutdown();
    initialize(width, height);
}

void DepthOfFieldEffect::shutdown() {
    if (!initialized_) return;

    coc_texture_        = INVALID_TEXTURE;
    near_field_         = INVALID_TEXTURE;
    far_field_          = INVALID_TEXTURE;
    near_field_blurred_ = INVALID_TEXTURE;
    far_field_blurred_  = INVALID_TEXTURE;
    initialized_ = false;
}

void DepthOfFieldEffect::execute(TextureHandle input_color,
                                   TextureHandle input_depth,
                                   TextureHandle output_color,
                                   float /*delta_time*/) {
    if (!initialized_ || !config_.enabled) return;

    // Step 1: Compute Circle of Confusion from depth
    // CoC = bokeh_radius * smoothstep(near/far ramp from depth)
    compute_coc(input_depth, coc_texture_);

    // Step 2: Separate color into near and far fields based on CoC
    separate_fields(input_color, coc_texture_, near_field_, far_field_);

    // Step 3: Blur each field independently
    // Disc blur with variable radius for bokeh approximation
    uint32_t half_w = std::max(width_ >> 1, 1u);
    uint32_t half_h = std::max(height_ >> 1, 1u);
    blur_field(near_field_, near_field_blurred_, half_w, half_h);
    blur_field(far_field_, far_field_blurred_, half_w, half_h);

    // Step 4: Composite: blend sharp, near-blur, and far-blur based on CoC
    composite_dof(input_color, near_field_blurred_, far_field_blurred_,
                  coc_texture_, output_color);
}

void DepthOfFieldEffect::compute_coc(TextureHandle /*depth*/, TextureHandle /*coc_out*/) {
    // GPU dispatch: dof_coc.comp
    //
    // float linearDepth = texture(depthTex, uv).r;
    //
    // // Near field CoC (negative for near blur)
    // float nearCoC = smoothstep(near_end, near_start, linearDepth);
    //
    // // Far field CoC (positive for far blur)
    // float farCoC = smoothstep(far_start, far_end, linearDepth);
    //
    // // Focus region has CoC = 0
    // float coc;
    // if (linearDepth < focus_distance - focus_range * 0.5)
    //     coc = -nearCoC * bokeh_radius;
    // else if (linearDepth > focus_distance + focus_range * 0.5)
    //     coc = farCoC * bokeh_radius;
    // else
    //     coc = 0.0;
    //
    // output = coc;
}

void DepthOfFieldEffect::separate_fields(TextureHandle /*color*/, TextureHandle /*coc*/,
                                            TextureHandle /*near_out*/, TextureHandle /*far_out*/) {
    // GPU dispatch: dof_separate.comp
    // near_out = color * saturate(-coc);  // only near-field contribution
    // far_out  = color * saturate(coc);   // only far-field contribution
}

void DepthOfFieldEffect::blur_field(TextureHandle /*input*/, TextureHandle /*output*/,
                                      uint32_t /*width*/, uint32_t /*height*/) {
    // GPU dispatch: dof_blur.comp
    // Poisson disc sampling with sample_count taps
    // Sample positions distributed on unit disc, scaled by CoC
    //
    // for (int i = 0; i < sample_count; i++) {
    //     vec2 offset = poissonDisc[i] * coc * texelSize;
    //     color += texture(input, uv + offset);
    // }
    // color /= sample_count;
}

void DepthOfFieldEffect::composite_dof(TextureHandle /*sharp*/, TextureHandle /*near_blur*/,
                                          TextureHandle /*far_blur*/, TextureHandle /*coc*/,
                                          TextureHandle /*output*/) {
    // GPU dispatch: dof_composite.comp
    // float cocValue = texture(cocTex, uv).r;
    // vec3 sharpColor = texture(sharpTex, uv).rgb;
    // vec3 nearColor  = texture(nearBlurTex, uv).rgb;
    // vec3 farColor   = texture(farBlurTex, uv).rgb;
    //
    // float nearWeight = saturate(-cocValue);
    // float farWeight  = saturate(cocValue);
    // float sharpWeight = 1.0 - nearWeight - farWeight;
    //
    // output = sharpColor * sharpWeight + nearColor * nearWeight + farColor * farWeight;
}

} // namespace pictor
