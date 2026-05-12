#pragma once

#include "pictor/surface/surface_provider.h"
#include <cstdint>
#include <vector>
#include <string>
#include <functional>

#ifdef PICTOR_HAS_VULKAN
#include <vulkan/vulkan.h>
#endif

namespace pictor {

/// Configuration for Vulkan instance / device creation.
struct VulkanContextConfig {
    const char* app_name    = "Pictor";
    uint32_t    app_version = 1;
    bool        validation  = false;  // VK_LAYER_KHRONOS_validation
};

/// Manages the Vulkan instance, physical/logical device, queue,
/// surface and swapchain.
///
/// Ownership model:
///   - VulkanContext **owns** VkInstance, VkDevice, VkSurfaceKHR, VkSwapchainKHR.
///   - The ISurfaceProvider is **borrowed** (host retains ownership).
class VulkanContext {
public:
    VulkanContext();
    ~VulkanContext();

    VulkanContext(const VulkanContext&) = delete;
    VulkanContext& operator=(const VulkanContext&) = delete;

    /// Full initialization: instance → surface → device → swapchain.
    bool initialize(ISurfaceProvider* provider, const VulkanContextConfig& cfg = {});

    /// Tear down everything in reverse order.
    void shutdown();

    /// Recreate swapchain (e.g. after window resize).
    bool recreate_swapchain();

    /// Acquire the next swapchain image. Returns image index, or UINT32_MAX on failure.
    uint32_t acquire_next_image();

    /// Present the given swapchain image.
    bool present(uint32_t image_index);

    /// Wait for device idle.
    void device_wait_idle();

    bool is_initialized() const { return initialized_; }

#ifdef PICTOR_HAS_VULKAN
    VkInstance       instance()        const { return instance_; }
    VkPhysicalDevice physical_device() const { return physical_device_; }
    VkDevice         device()          const { return device_; }
    VkQueue          graphics_queue()  const { return graphics_queue_; }
    uint32_t         queue_family()    const { return queue_family_index_; }
    VkSwapchainKHR   swapchain()       const { return swapchain_; }
    VkFormat         swapchain_format()const { return swapchain_format_; }
    VkExtent2D       swapchain_extent()const { return swapchain_extent_; }
    const std::vector<VkImageView>& swapchain_image_views() const { return swapchain_image_views_; }
    VkRenderPass     default_render_pass() const { return render_pass_; }
    const std::vector<VkFramebuffer>& framebuffers() const { return framebuffers_; }
    VkCommandPool    command_pool() const { return command_pool_; }
    const std::vector<VkCommandBuffer>& command_buffers() const { return command_buffers_; }

    // Single-time command buffer helpers (for uploads, layout transitions, etc.)
    VkCommandBuffer begin_single_time_commands();
    void            end_single_time_commands(VkCommandBuffer cmd);

    // Per-frame sync objects
    VkSemaphore image_available_semaphore() const { return image_available_sem_; }
    VkSemaphore render_finished_semaphore() const { return render_finished_sem_; }
    VkFence     in_flight_fence()           const { return in_flight_fence_; }

    // Optional extension features that the device was created with. Pictor
    // probes these during device creation and enables whichever are
    // supported. Consumers (e.g. the Rive renderer wrapper) query these to
    // pick the most efficient GPU path.
    //
    // fragment_shader_interlock (VK_EXT_fragment_shader_interlock):
    //   Lets Rive use pixel-interlock mode — single-pass coverage without
    //   atomic compute. Supported on NVIDIA Turing+, Intel Gen9+, AMD RDNA+.
    //
    // rasterization_order_attachment_access
    //   (VK_EXT_rasterization_order_attachment_access):
    //   Lets Rive use raster-ordered ROV — also single-pass, slightly
    //   cheaper than interlock on some drivers.
    bool has_fragment_shader_interlock()               const { return has_fragment_shader_interlock_; }
    bool has_rasterization_order_attachment_access()   const { return has_rasterization_order_attachment_access_; }
#endif

private:
#ifdef PICTOR_HAS_VULKAN
    bool create_instance(const VulkanContextConfig& cfg);
    bool create_surface();
    bool pick_physical_device();
    bool create_logical_device();
    bool create_swapchain();
    bool create_image_views();
    bool create_render_pass();
    bool create_framebuffers();
    bool create_command_pool_and_buffers();
    bool create_sync_objects();
    void cleanup_swapchain();

    ISurfaceProvider* provider_ = nullptr;

    VkInstance               instance_           = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT debug_messenger_    = VK_NULL_HANDLE;
    VkSurfaceKHR             surface_            = VK_NULL_HANDLE;
    VkPhysicalDevice         physical_device_    = VK_NULL_HANDLE;
    VkDevice                 device_             = VK_NULL_HANDLE;
    VkQueue                  graphics_queue_     = VK_NULL_HANDLE;
    VkQueue                  present_queue_      = VK_NULL_HANDLE;
    uint32_t                 queue_family_index_ = UINT32_MAX;

    VkSwapchainKHR           swapchain_          = VK_NULL_HANDLE;
    VkFormat                 swapchain_format_   = VK_FORMAT_B8G8R8A8_SRGB;
    VkExtent2D               swapchain_extent_   = {0, 0};
    std::vector<VkImage>     swapchain_images_;
    std::vector<VkImageView> swapchain_image_views_;

    VkRenderPass             render_pass_        = VK_NULL_HANDLE;
    std::vector<VkFramebuffer> framebuffers_;

    VkCommandPool            command_pool_       = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> command_buffers_;

    VkSemaphore              image_available_sem_ = VK_NULL_HANDLE;
    VkSemaphore              render_finished_sem_ = VK_NULL_HANDLE;
    VkFence                  in_flight_fence_     = VK_NULL_HANDLE;

    bool has_fragment_shader_interlock_             = false;
    bool has_rasterization_order_attachment_access_ = false;
#endif

    bool initialized_ = false;
};

} // namespace pictor
