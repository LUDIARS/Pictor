#pragma once

#include <string>
#include <vector>

namespace pictor {

/// Name of the device extension a portability (non-conformant, e.g. MoltenVK)
/// implementation exposes. When advertised it must be enabled.
inline constexpr const char* kPortabilitySubsetExtensionName = "VK_KHR_portability_subset";

/// Feature bits of VkPhysicalDevicePortabilitySubsetFeaturesKHR, mirrored so
/// hosts can state requirements without the provisional Vulkan header.
struct PortabilityFeatures {
    bool constant_alpha_color_blend_factors        = false;
    bool events                                    = false;
    bool image_view_format_reinterpretation        = false;
    bool image_view_format_swizzle                 = false;
    bool image_view_2d_on_3d_image                 = false;
    bool multisample_array_image                   = false;
    bool mutable_comparison_samplers               = false;
    bool point_polygons                            = false;
    bool sampler_mip_lod_bias                      = false;
    bool separate_stencil_mask_ref                 = false;
    bool shader_sample_rate_interpolation_functions = false;
    bool tessellation_isolines                     = false;
    bool tessellation_point_mode                   = false;
    bool triangle_fans                             = false;
    bool vertex_attribute_access_beyond_stride     = false;
};

/// Portability capabilities probed at context creation.
struct PortabilityCapabilities {
    /// VK_KHR_portability_enumeration was enabled on the instance.
    bool                instance_enumeration = false;
    /// The selected device is a portability subset device; only then do the
    /// feature bits below restrict anything.
    bool                subset_device        = false;
    PortabilityFeatures features{};
};

/// First required extension absent from `available`, or nullptr.
const char* first_missing_extension(const std::vector<const char*>& required,
                                    const std::vector<std::string>& available);

/// First feature `required` asks for that a portability subset device does not
/// support, or nullptr. A conformant (non-subset) device supports all of them.
const char* first_missing_portability_feature(const PortabilityCapabilities& caps,
                                              const PortabilityFeatures& required);

} // namespace pictor
