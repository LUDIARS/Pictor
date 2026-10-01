#ifdef PICTOR_HAS_VULKAN

#include "vulkan_portability_probe.h"

#include <algorithm>

namespace pictor {

namespace {

// VK_KHR_portability_subset is provisional: vulkan.h only declares its feature
// struct behind VK_ENABLE_BETA_EXTENSIONS, and enabling that in one translation
// unit would give VkStructureType a different definition there. Mirror the
// registry layout instead (extension 164, revision 1) so every SDK and NDK
// header set can query the feature bits.
constexpr VkStructureType kPortabilitySubsetFeaturesSType =
    static_cast<VkStructureType>(1000163000);

struct PortabilitySubsetFeatures {
    VkStructureType sType = kPortabilitySubsetFeaturesSType;
    void*           pNext = nullptr;
    VkBool32        constantAlphaColorBlendFactors = VK_FALSE;
    VkBool32        events = VK_FALSE;
    VkBool32        imageViewFormatReinterpretation = VK_FALSE;
    VkBool32        imageViewFormatSwizzle = VK_FALSE;
    VkBool32        imageView2DOn3DImage = VK_FALSE;
    VkBool32        multisampleArrayImage = VK_FALSE;
    VkBool32        mutableComparisonSamplers = VK_FALSE;
    VkBool32        pointPolygons = VK_FALSE;
    VkBool32        samplerMipLodBias = VK_FALSE;
    VkBool32        separateStencilMaskRef = VK_FALSE;
    VkBool32        shaderSampleRateInterpolationFunctions = VK_FALSE;
    VkBool32        tessellationIsolines = VK_FALSE;
    VkBool32        tessellationPointMode = VK_FALSE;
    VkBool32        triangleFans = VK_FALSE;
    VkBool32        vertexAttributeAccessBeyondStride = VK_FALSE;
};

bool listed(const std::vector<std::string>& names, const char* name) {
    return std::any_of(names.begin(), names.end(),
        [name](const std::string& n) { return n == name; });
}

} // namespace

std::vector<std::string> enumerate_instance_extension_names() {
    std::vector<std::string> names;
    uint32_t count = 0;
    if (vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr) != VK_SUCCESS) {
        return names;
    }
    std::vector<VkExtensionProperties> props(count);
    // VK_INCOMPLETE (list grew between calls) still fills `count` entries.
    if (count > 0) vkEnumerateInstanceExtensionProperties(nullptr, &count, props.data());
    props.resize(count);
    names.reserve(count);
    for (const VkExtensionProperties& p : props) names.emplace_back(p.extensionName);
    return names;
}

std::vector<std::string> enumerate_device_extension_names(VkPhysicalDevice device) {
    std::vector<std::string> names;
    if (device == VK_NULL_HANDLE) return names;
    uint32_t count = 0;
    if (vkEnumerateDeviceExtensionProperties(device, nullptr, &count, nullptr) != VK_SUCCESS) {
        return names;
    }
    std::vector<VkExtensionProperties> props(count);
    if (count > 0) vkEnumerateDeviceExtensionProperties(device, nullptr, &count, props.data());
    props.resize(count);
    names.reserve(count);
    for (const VkExtensionProperties& p : props) names.emplace_back(p.extensionName);
    return names;
}

const char* portability_enumeration_extension_name() {
#ifdef VK_KHR_portability_enumeration
    return VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME;
#else
    return "";
#endif
}

VkInstanceCreateFlags portability_enumeration_create_flags() {
#ifdef VK_KHR_portability_enumeration
    return VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
#else
    return 0;
#endif
}

PortabilityCapabilities probe_portability(VkPhysicalDevice device,
                                          const std::vector<std::string>& device_extensions,
                                          bool instance_enumeration) {
    PortabilityCapabilities caps;
    caps.instance_enumeration = instance_enumeration;
    caps.subset_device = listed(device_extensions, kPortabilitySubsetExtensionName);
    if (!caps.subset_device || device == VK_NULL_HANDLE) return caps;

    PortabilitySubsetFeatures subset{};
    VkPhysicalDeviceFeatures2 features2{};
    features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    features2.pNext = &subset;
    vkGetPhysicalDeviceFeatures2(device, &features2);

    PortabilityFeatures& f = caps.features;
    f.constant_alpha_color_blend_factors = subset.constantAlphaColorBlendFactors == VK_TRUE;
    f.events                             = subset.events == VK_TRUE;
    f.image_view_format_reinterpretation = subset.imageViewFormatReinterpretation == VK_TRUE;
    f.image_view_format_swizzle          = subset.imageViewFormatSwizzle == VK_TRUE;
    f.image_view_2d_on_3d_image          = subset.imageView2DOn3DImage == VK_TRUE;
    f.multisample_array_image            = subset.multisampleArrayImage == VK_TRUE;
    f.mutable_comparison_samplers        = subset.mutableComparisonSamplers == VK_TRUE;
    f.point_polygons                     = subset.pointPolygons == VK_TRUE;
    f.sampler_mip_lod_bias               = subset.samplerMipLodBias == VK_TRUE;
    f.separate_stencil_mask_ref          = subset.separateStencilMaskRef == VK_TRUE;
    f.shader_sample_rate_interpolation_functions =
        subset.shaderSampleRateInterpolationFunctions == VK_TRUE;
    f.tessellation_isolines              = subset.tessellationIsolines == VK_TRUE;
    f.tessellation_point_mode            = subset.tessellationPointMode == VK_TRUE;
    f.triangle_fans                      = subset.triangleFans == VK_TRUE;
    f.vertex_attribute_access_beyond_stride =
        subset.vertexAttributeAccessBeyondStride == VK_TRUE;
    return caps;
}

} // namespace pictor

#endif // PICTOR_HAS_VULKAN
