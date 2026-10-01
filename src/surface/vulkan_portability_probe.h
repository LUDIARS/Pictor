#pragma once

#include "pictor/surface/vulkan_capability_check.h"

#include <string>
#include <vector>
#include <vulkan/vulkan.h>

namespace pictor {

/// Instance-level extension names the loader reports.
std::vector<std::string> enumerate_instance_extension_names();

/// Device-level extension names the physical device reports.
std::vector<std::string> enumerate_device_extension_names(VkPhysicalDevice device);

/// Instance extension that lets the loader list portability (e.g. MoltenVK)
/// devices. Empty when the Vulkan headers predate it.
const char* portability_enumeration_extension_name();

/// Instance create flag paired with portability_enumeration_extension_name().
VkInstanceCreateFlags portability_enumeration_create_flags();

/// Probes VK_KHR_portability_subset on `device`. When the extension is
/// advertised, the subset feature bits are queried through
/// vkGetPhysicalDeviceFeatures2.
PortabilityCapabilities probe_portability(VkPhysicalDevice device,
                                          const std::vector<std::string>& device_extensions,
                                          bool instance_enumeration);

} // namespace pictor
