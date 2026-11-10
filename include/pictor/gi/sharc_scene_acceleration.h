#pragma once

#ifndef PICTOR_HAS_SHARC_VULKAN
#error "SHaRC Vulkan requires linking Pictor::sharc_vulkan (PICTOR_ENABLE_SHARC_VULKAN=ON)"
#endif

/// SharcSceneAcceleration — the host's static geometry as acceleration
/// structures for the irradiance cache's ray queries: one bottom level per
/// distinct mesh range, one top level placing each placement with its
/// transform, and the instance table a hit reads to find its triangle and
/// surface (SharcRayInstanceGpu, indexed by the instance's custom index).
///
/// The host keeps its vertex and index buffers (created with
/// SHADER_DEVICE_ADDRESS, ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY and
/// STORAGE_BUFFER usage, memory allocated with DEVICE_ADDRESS); positions are
/// three floats at the start of each vertex. Requires a context created with
/// VulkanConfig::require_ray_query.

#include "pictor/gi/sharc_irradiance_types.h"

#include <cstdint>
#include <vector>

#ifdef PICTOR_HAS_VULKAN
#include <vulkan/vulkan.h>
#endif

namespace pictor {

class VulkanContext;

struct SharcGeometryBuffers {
#ifdef PICTOR_HAS_VULKAN
    VkBuffer vertices = VK_NULL_HANDLE;
    VkBuffer indices = VK_NULL_HANDLE;  ///< uint32
#endif
    uint32_t vertex_stride = 0;         ///< bytes
};

class SharcSceneAcceleration {
public:
    SharcSceneAcceleration() = default;
    ~SharcSceneAcceleration();
    SharcSceneAcceleration(const SharcSceneAcceleration&) = delete;
    SharcSceneAcceleration& operator=(const SharcSceneAcceleration&) = delete;

#ifdef PICTOR_HAS_VULKAN
    /// `indices` are the index buffer's contents (to bound each range's vertices).
    bool build(VulkanContext& vk, const SharcGeometryBuffers& geometry, const std::vector<uint32_t>& indices,
               const std::vector<SharcPlacement>& placements);

    bool ready() const { return top_.handle != VK_NULL_HANDLE; }
    VkAccelerationStructureKHR top_level() const { return top_.handle; }
    VkBuffer instance_table() const { return instance_table_.buffer; }
    VkDeviceSize instance_table_bytes() const { return instance_table_.size; }
#endif
    void shutdown();

private:
#ifdef PICTOR_HAS_VULKAN
    struct Buffer {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkDeviceSize size = 0;
        void* mapped = nullptr;
    };
    struct Structure {
        VkAccelerationStructureKHR handle = VK_NULL_HANDLE;
        Buffer storage;
    };

    bool load_functions_();
    bool create_buffer_(VkDeviceSize size, VkBufferUsageFlags usage, bool host_visible, Buffer& out);
    void destroy_buffer_(Buffer& b);
    VkDeviceAddress address_of_(VkBuffer buffer) const;
    bool create_structure_(VkAccelerationStructureTypeKHR type, VkDeviceSize size, Structure& out);
    void destroy_structure_(Structure& s);

    VulkanContext* vk_ = nullptr;
    VkDevice device_ = VK_NULL_HANDLE;
    std::vector<Structure> bottom_;
    Structure top_;
    Buffer instance_table_;

    PFN_vkGetAccelerationStructureBuildSizesKHR get_build_sizes_ = nullptr;
    PFN_vkCreateAccelerationStructureKHR create_ = nullptr;
    PFN_vkDestroyAccelerationStructureKHR destroy_ = nullptr;
    PFN_vkCmdBuildAccelerationStructuresKHR cmd_build_ = nullptr;
    PFN_vkGetAccelerationStructureDeviceAddressKHR get_structure_address_ = nullptr;
#endif
};

}  // namespace pictor
