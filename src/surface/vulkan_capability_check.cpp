#include "pictor/surface/vulkan_capability_check.h"

#include <algorithm>

namespace pictor {

const char* first_missing_extension(const std::vector<const char*>& required,
                                    const std::vector<std::string>& available) {
    for (const char* name : required) {
        if (!name) continue;
        const bool present = std::any_of(available.begin(), available.end(),
            [name](const std::string& a) { return a == name; });
        if (!present) return name;
    }
    return nullptr;
}

const char* first_missing_portability_feature(const PortabilityCapabilities& caps,
                                              const PortabilityFeatures& required) {
    if (!caps.subset_device) return nullptr;

    struct Entry {
        bool PortabilityFeatures::* member;
        const char* name;
    };
    static constexpr Entry kEntries[] = {
        {&PortabilityFeatures::constant_alpha_color_blend_factors, "constantAlphaColorBlendFactors"},
        {&PortabilityFeatures::events,                             "events"},
        {&PortabilityFeatures::image_view_format_reinterpretation, "imageViewFormatReinterpretation"},
        {&PortabilityFeatures::image_view_format_swizzle,          "imageViewFormatSwizzle"},
        {&PortabilityFeatures::image_view_2d_on_3d_image,          "imageView2DOn3DImage"},
        {&PortabilityFeatures::multisample_array_image,            "multisampleArrayImage"},
        {&PortabilityFeatures::mutable_comparison_samplers,        "mutableComparisonSamplers"},
        {&PortabilityFeatures::point_polygons,                     "pointPolygons"},
        {&PortabilityFeatures::sampler_mip_lod_bias,               "samplerMipLodBias"},
        {&PortabilityFeatures::separate_stencil_mask_ref,          "separateStencilMaskRef"},
        {&PortabilityFeatures::shader_sample_rate_interpolation_functions,
                                                                   "shaderSampleRateInterpolationFunctions"},
        {&PortabilityFeatures::tessellation_isolines,              "tessellationIsolines"},
        {&PortabilityFeatures::tessellation_point_mode,            "tessellationPointMode"},
        {&PortabilityFeatures::triangle_fans,                      "triangleFans"},
        {&PortabilityFeatures::vertex_attribute_access_beyond_stride,
                                                                   "vertexAttributeAccessBeyondStride"},
    };
    for (const Entry& e : kEntries) {
        if (required.*(e.member) && !(caps.features.*(e.member))) return e.name;
    }
    return nullptr;
}

} // namespace pictor
