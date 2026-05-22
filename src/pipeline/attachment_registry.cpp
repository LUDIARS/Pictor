#include "pictor/pipeline/attachment_registry.h"

#include <algorithm>
#include <cassert>
#include <cstring>

namespace pictor {

// ============================================================
// Vulkan-free portion (always built)
// ============================================================

AttachmentRegistry::AttachmentRegistry() = default;

AttachmentRegistry::~AttachmentRegistry() {
#ifdef PICTOR_HAS_VULKAN
    shutdown_vulkan();
#endif
}

void AttachmentRegistry::set_defs(std::span<const AttachmentDef> defs) {
    defs_.assign(defs.begin(), defs.end());
#ifdef PICTOR_HAS_VULKAN
    // Resources are stale — must call initialize_vulkan() again before use.
    if (!resources_.empty()) {
        shutdown_vulkan();
    }
#endif
}

uint16_t AttachmentRegistry::index_of(std::string_view name) const {
    for (size_t i = 0; i < defs_.size(); ++i) {
        if (defs_[i].name == name) {
            return static_cast<uint16_t>(i);
        }
    }
    return INVALID_INDEX;
}

#ifdef PICTOR_HAS_VULKAN

// ============================================================
// Vulkan format / layout / op translation
// ============================================================

VkFormat to_vk_format(AttachmentFormat f) {
    switch (f) {
        case AttachmentFormat::UNDEFINED:           return VK_FORMAT_UNDEFINED;
        case AttachmentFormat::R8G8B8A8_UNORM:      return VK_FORMAT_R8G8B8A8_UNORM;
        case AttachmentFormat::R8G8B8A8_SRGB:       return VK_FORMAT_R8G8B8A8_SRGB;
        case AttachmentFormat::B8G8R8A8_UNORM:      return VK_FORMAT_B8G8R8A8_UNORM;
        case AttachmentFormat::B8G8R8A8_SRGB:       return VK_FORMAT_B8G8R8A8_SRGB;
        case AttachmentFormat::R16G16B16A16_SFLOAT: return VK_FORMAT_R16G16B16A16_SFLOAT;
        case AttachmentFormat::R32G32B32A32_SFLOAT: return VK_FORMAT_R32G32B32A32_SFLOAT;
        case AttachmentFormat::R11G11B10_UFLOAT:    return VK_FORMAT_B10G11R11_UFLOAT_PACK32;
        case AttachmentFormat::D16_UNORM:           return VK_FORMAT_D16_UNORM;
        case AttachmentFormat::D24_UNORM_S8_UINT:   return VK_FORMAT_D24_UNORM_S8_UINT;
        case AttachmentFormat::D32_SFLOAT:          return VK_FORMAT_D32_SFLOAT;
        case AttachmentFormat::D32_SFLOAT_S8_UINT:  return VK_FORMAT_D32_SFLOAT_S8_UINT;
    }
    return VK_FORMAT_UNDEFINED;
}

VkImageUsageFlags to_vk_usage(uint32_t mask) {
    VkImageUsageFlags v = 0;
    if (mask & USAGE_TRANSFER_SRC)              v |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (mask & USAGE_TRANSFER_DST)              v |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (mask & USAGE_SAMPLED)                   v |= VK_IMAGE_USAGE_SAMPLED_BIT;
    if (mask & USAGE_STORAGE)                   v |= VK_IMAGE_USAGE_STORAGE_BIT;
    if (mask & USAGE_COLOR_ATTACHMENT)          v |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (mask & USAGE_DEPTH_STENCIL_ATTACHMENT)  v |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    if (mask & USAGE_TRANSIENT_ATTACHMENT)      v |= VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT;
    if (mask & USAGE_INPUT_ATTACHMENT)          v |= VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT;
    return v;
}

VkAttachmentLoadOp to_vk_load_op(AttachmentLoadOp op) {
    switch (op) {
        case AttachmentLoadOp::LOAD:      return VK_ATTACHMENT_LOAD_OP_LOAD;
        case AttachmentLoadOp::CLEAR:     return VK_ATTACHMENT_LOAD_OP_CLEAR;
        case AttachmentLoadOp::DONT_CARE: return VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        case AttachmentLoadOp::NONE:      return VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    }
    return VK_ATTACHMENT_LOAD_OP_CLEAR;
}

VkAttachmentStoreOp to_vk_store_op(AttachmentStoreOp op) {
    switch (op) {
        case AttachmentStoreOp::STORE:     return VK_ATTACHMENT_STORE_OP_STORE;
        case AttachmentStoreOp::DONT_CARE: return VK_ATTACHMENT_STORE_OP_DONT_CARE;
        case AttachmentStoreOp::NONE:      return VK_ATTACHMENT_STORE_OP_DONT_CARE;
    }
    return VK_ATTACHMENT_STORE_OP_STORE;
}

VkImageLayout to_vk_layout(ImageLayout l) {
    switch (l) {
        case ImageLayout::UNDEFINED:                        return VK_IMAGE_LAYOUT_UNDEFINED;
        case ImageLayout::GENERAL:                          return VK_IMAGE_LAYOUT_GENERAL;
        case ImageLayout::COLOR_ATTACHMENT_OPTIMAL:         return VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        case ImageLayout::DEPTH_STENCIL_ATTACHMENT_OPTIMAL: return VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        case ImageLayout::DEPTH_STENCIL_READ_ONLY_OPTIMAL:  return VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
        case ImageLayout::SHADER_READ_ONLY_OPTIMAL:         return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        case ImageLayout::TRANSFER_SRC_OPTIMAL:             return VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        case ImageLayout::TRANSFER_DST_OPTIMAL:             return VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        case ImageLayout::PRESENT_SRC_KHR:                  return VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    }
    return VK_IMAGE_LAYOUT_UNDEFINED;
}

// ============================================================
// GPU resource lifecycle
// ============================================================

namespace {

uint32_t find_memory_type(VkPhysicalDevice phys, uint32_t type_bits,
                          VkMemoryPropertyFlags want) {
    VkPhysicalDeviceMemoryProperties mp{};
    vkGetPhysicalDeviceMemoryProperties(phys, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
        if ((type_bits & (1u << i)) &&
            (mp.memoryTypes[i].propertyFlags & want) == want) {
            return i;
        }
    }
    return UINT32_MAX;
}

VkExtent2D resolve_extent(const AttachmentDef& d, VkExtent2D swap) {
    if (d.sizing == AttachmentSizing::SWAPCHAIN_RELATIVE) {
        VkExtent2D e;
        e.width  = static_cast<uint32_t>(static_cast<float>(swap.width)  * d.scale);
        e.height = static_cast<uint32_t>(static_cast<float>(swap.height) * d.scale);
        if (e.width  == 0) e.width  = 1;
        if (e.height == 0) e.height = 1;
        return e;
    }
    return { d.width, d.height };
}

VkImageAspectFlags aspect_of(const AttachmentDef& d) {
    if (is_depth_format(d.format)) {
        VkImageAspectFlags a = VK_IMAGE_ASPECT_DEPTH_BIT;
        if (d.format == AttachmentFormat::D24_UNORM_S8_UINT ||
            d.format == AttachmentFormat::D32_SFLOAT_S8_UINT) {
            a |= VK_IMAGE_ASPECT_STENCIL_BIT;
        }
        return a;
    }
    return VK_IMAGE_ASPECT_COLOR_BIT;
}

} // anon

bool AttachmentRegistry::create_one_(uint16_t index, VkExtent2D swap_extent) {
    const AttachmentDef& d = defs_[index];
    Resource& r = resources_[index];

    if (d.kind == AttachmentKind::SWAPCHAIN_COLOR) {
        // Owned by VulkanContext — populated via set_swapchain_images().
        r.is_swapchain = true;
        r.vk_format    = to_vk_format(d.format);
        r.extent       = swap_extent;
        return true;
    }

    r.is_swapchain = false;
    r.vk_format    = to_vk_format(d.format);
    r.extent       = resolve_extent(d, swap_extent);

    const uint32_t slots = flight_count_;
    r.images.assign(slots, VK_NULL_HANDLE);
    r.views.assign(slots, VK_NULL_HANDLE);
    r.memories.assign(slots, VK_NULL_HANDLE);

    for (uint32_t i = 0; i < slots; ++i) {
        VkImageCreateInfo ic{};
        ic.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ic.imageType     = VK_IMAGE_TYPE_2D;
        ic.format        = r.vk_format;
        ic.extent        = { r.extent.width, r.extent.height, 1 };
        ic.mipLevels     = 1;
        ic.arrayLayers   = 1;
        ic.samples       = VK_SAMPLE_COUNT_1_BIT;
        ic.tiling        = VK_IMAGE_TILING_OPTIMAL;
        ic.usage         = to_vk_usage(d.usage);
        ic.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
        ic.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        if (vkCreateImage(device_, &ic, nullptr, &r.images[i]) != VK_SUCCESS) {
            std::fprintf(stderr, "[AttachmentRegistry] vkCreateImage failed (%s slot %u)\n",
                         d.name.c_str(), i);
            return false;
        }

        VkMemoryRequirements mreq{};
        vkGetImageMemoryRequirements(device_, r.images[i], &mreq);
        VkMemoryAllocateInfo ai{};
        ai.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        ai.allocationSize  = mreq.size;
        ai.memoryTypeIndex = find_memory_type(physical_device_, mreq.memoryTypeBits,
                                              VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (ai.memoryTypeIndex == UINT32_MAX) {
            std::fprintf(stderr, "[AttachmentRegistry] no device-local memory type\n");
            return false;
        }
        if (vkAllocateMemory(device_, &ai, nullptr, &r.memories[i]) != VK_SUCCESS) {
            std::fprintf(stderr, "[AttachmentRegistry] vkAllocateMemory failed\n");
            return false;
        }
        vkBindImageMemory(device_, r.images[i], r.memories[i], 0);

        VkImageViewCreateInfo iv{};
        iv.sType                           = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        iv.image                           = r.images[i];
        iv.viewType                        = VK_IMAGE_VIEW_TYPE_2D;
        iv.format                          = r.vk_format;
        iv.subresourceRange.aspectMask     = aspect_of(d);
        iv.subresourceRange.baseMipLevel   = 0;
        iv.subresourceRange.levelCount     = 1;
        iv.subresourceRange.baseArrayLayer = 0;
        iv.subresourceRange.layerCount     = 1;

        if (vkCreateImageView(device_, &iv, nullptr, &r.views[i]) != VK_SUCCESS) {
            std::fprintf(stderr, "[AttachmentRegistry] vkCreateImageView failed\n");
            return false;
        }
    }
    return true;
}

void AttachmentRegistry::destroy_one_(Resource& r) {
    if (r.is_swapchain) {
        // Not owned — VulkanContext owns swapchain images/views.
        r.images.clear();
        r.views.clear();
        r.memories.clear();
        return;
    }
    for (size_t i = 0; i < r.views.size(); ++i) {
        if (r.views[i]) vkDestroyImageView(device_, r.views[i], nullptr);
    }
    for (size_t i = 0; i < r.images.size(); ++i) {
        if (r.images[i]) vkDestroyImage(device_, r.images[i], nullptr);
    }
    for (size_t i = 0; i < r.memories.size(); ++i) {
        if (r.memories[i]) vkFreeMemory(device_, r.memories[i], nullptr);
    }
    r.views.clear();
    r.images.clear();
    r.memories.clear();
}

bool AttachmentRegistry::initialize_vulkan(VkPhysicalDevice physical_device,
                                           VkDevice         device,
                                           VkExtent2D       swapchain_extent,
                                           uint32_t         flight_count)
{
    physical_device_  = physical_device;
    device_           = device;
    current_extent_   = swapchain_extent;
    flight_count_     = std::min(flight_count, MAX_FLIGHTS);
    swapchain_image_count_ = 0; // populated by set_swapchain_images()

    resources_.clear();
    resources_.resize(defs_.size());

    for (uint16_t i = 0; i < static_cast<uint16_t>(defs_.size()); ++i) {
        if (!create_one_(i, swapchain_extent)) return false;
    }
    return true;
}

bool AttachmentRegistry::resize(VkExtent2D new_extent) {
    if (!device_) return false;
    if (new_extent.width == current_extent_.width && new_extent.height == current_extent_.height) {
        return true;
    }

    // Destroy all non-swapchain images and recreate at new extent.
    for (size_t i = 0; i < resources_.size(); ++i) {
        if (!resources_[i].is_swapchain) {
            destroy_one_(resources_[i]);
        }
    }
    current_extent_ = new_extent;
    for (uint16_t i = 0; i < static_cast<uint16_t>(defs_.size()); ++i) {
        if (!resources_[i].is_swapchain) {
            if (!create_one_(i, new_extent)) return false;
        } else {
            // Swapchain entries: clear handles, host re-injects.
            resources_[i].images.clear();
            resources_[i].views.clear();
            resources_[i].extent = new_extent;
        }
    }
    return true;
}

void AttachmentRegistry::set_swapchain_images(std::span<const VkImage>     images,
                                              std::span<const VkImageView> views,
                                              VkFormat                     format,
                                              VkExtent2D                   extent)
{
    swapchain_image_count_ = static_cast<uint32_t>(images.size());
    for (size_t i = 0; i < resources_.size(); ++i) {
        if (!resources_[i].is_swapchain) continue;
        resources_[i].images.assign(images.begin(), images.end());
        resources_[i].views.assign(views.begin(), views.end());
        resources_[i].vk_format = format;
        resources_[i].extent    = extent;
    }
}

void AttachmentRegistry::shutdown_vulkan() {
    if (!device_) {
        resources_.clear();
        return;
    }
    for (auto& r : resources_) destroy_one_(r);
    resources_.clear();
    device_ = VK_NULL_HANDLE;
    physical_device_ = VK_NULL_HANDLE;
}

VkImageView AttachmentRegistry::view(uint16_t att_index, uint32_t slot) const {
    const Resource& r = resources_[att_index];
    if (slot >= r.views.size()) return VK_NULL_HANDLE;
    return r.views[slot];
}

VkImage AttachmentRegistry::image(uint16_t att_index, uint32_t slot) const {
    const Resource& r = resources_[att_index];
    if (slot >= r.images.size()) return VK_NULL_HANDLE;
    return r.images[slot];
}

VkFormat AttachmentRegistry::vk_format(uint16_t att_index) const {
    return resources_[att_index].vk_format;
}

VkExtent2D AttachmentRegistry::extent(uint16_t att_index) const {
    return resources_[att_index].extent;
}

uint32_t AttachmentRegistry::slot_count(uint16_t att_index) const {
    return static_cast<uint32_t>(resources_[att_index].images.size());
}

#endif // PICTOR_HAS_VULKAN

} // namespace pictor
