#include "pictor/surface/vulkan_context.h"

#ifdef PICTOR_HAS_VULKAN

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

namespace pictor {

// ---------- helpers ----------

static VKAPI_ATTR VkBool32 VKAPI_CALL debug_callback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT /*type*/,
    const VkDebugUtilsMessengerCallbackDataEXT* data,
    void* /*user*/)
{
    if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        fprintf(stderr, "[Vulkan Validation] %s\n", data->pMessage);
    }
    return VK_FALSE;
}

// ---------- public ----------

VulkanContext::VulkanContext() = default;

VulkanContext::~VulkanContext() {
    if (initialized_) shutdown();
}

bool VulkanContext::initialize(ISurfaceProvider* provider,
                               const VulkanContextConfig& cfg)
{
    if (!provider) return false;
    provider_ = provider;

    if (!create_instance(cfg))              return false;
    if (!create_surface())                  return false;
    if (!pick_physical_device())            return false;
    if (!create_logical_device())           return false;
    if (!create_swapchain())                return false;
    if (!create_image_views())              return false;
    if (!create_render_pass())              return false;
    if (!create_framebuffers())             return false;
    if (!create_command_pool_and_buffers()) return false;
    if (!create_sync_objects())             return false;

    initialized_ = true;
    return true;
}

void VulkanContext::shutdown() {
    if (!initialized_) return;

    vkDeviceWaitIdle(device_);

    // Sync objects
    if (render_finished_sem_) vkDestroySemaphore(device_, render_finished_sem_, nullptr);
    if (image_available_sem_) vkDestroySemaphore(device_, image_available_sem_, nullptr);
    if (in_flight_fence_)     vkDestroyFence(device_, in_flight_fence_, nullptr);

    // Command pool (frees command buffers implicitly)
    if (command_pool_) vkDestroyCommandPool(device_, command_pool_, nullptr);

    cleanup_swapchain();

    if (device_)  vkDestroyDevice(device_, nullptr);
    if (surface_) vkDestroySurfaceKHR(instance_, surface_, nullptr);

    if (debug_messenger_) {
        auto func = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(instance_, "vkDestroyDebugUtilsMessengerEXT"));
        if (func) func(instance_, debug_messenger_, nullptr);
    }

    if (instance_) vkDestroyInstance(instance_, nullptr);

    initialized_ = false;
}

bool VulkanContext::recreate_swapchain() {
    vkDeviceWaitIdle(device_);
    cleanup_swapchain();

    if (!create_swapchain())    return false;
    if (!create_image_views())  return false;
    if (!create_render_pass())  return false;
    if (!create_framebuffers()) return false;

    return true;
}

uint32_t VulkanContext::acquire_next_image() {
    vkWaitForFences(device_, 1, &in_flight_fence_, VK_TRUE, UINT64_MAX);
    vkResetFences(device_, 1, &in_flight_fence_);

    uint32_t index = 0;
    VkResult result = vkAcquireNextImageKHR(
        device_, swapchain_, UINT64_MAX,
        image_available_sem_, VK_NULL_HANDLE, &index);

    if (result == VK_ERROR_OUT_OF_DATE_KHR) {
        recreate_swapchain();
        return UINT32_MAX;
    }
    return index;
}

bool VulkanContext::present(uint32_t image_index) {
    VkPresentInfoKHR info{};
    info.sType              = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    info.waitSemaphoreCount = 1;
    info.pWaitSemaphores    = &render_finished_sem_;
    info.swapchainCount     = 1;
    info.pSwapchains        = &swapchain_;
    info.pImageIndices      = &image_index;

    VkResult result = vkQueuePresentKHR(present_queue_, &info);
    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) {
        recreate_swapchain();
    }
    return result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR;
}

void VulkanContext::device_wait_idle() {
    if (device_) vkDeviceWaitIdle(device_);
}

// ---------- private: instance ----------

bool VulkanContext::create_instance(const VulkanContextConfig& cfg) {
    VkApplicationInfo app_info{};
    app_info.sType              = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app_info.pApplicationName   = cfg.app_name;
    app_info.applicationVersion = cfg.app_version;
    app_info.pEngineName        = "Pictor";
    app_info.engineVersion      = VK_MAKE_VERSION(2, 1, 0);
    app_info.apiVersion         = VK_API_VERSION_1_2;

    // Collect extensions from provider
    const char* ext_names[16] = {};
    uint32_t ext_count = provider_->get_required_instance_extensions(ext_names, 16);

    std::vector<const char*> extensions(ext_names, ext_names + ext_count);

    std::vector<const char*> layers;
    if (cfg.validation) {
        layers.push_back("VK_LAYER_KHRONOS_validation");
        extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    }

    VkInstanceCreateInfo create_info{};
    create_info.sType                   = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    create_info.pApplicationInfo        = &app_info;
    create_info.enabledExtensionCount   = static_cast<uint32_t>(extensions.size());
    create_info.ppEnabledExtensionNames = extensions.data();
    create_info.enabledLayerCount       = static_cast<uint32_t>(layers.size());
    create_info.ppEnabledLayerNames     = layers.data();

    if (vkCreateInstance(&create_info, nullptr, &instance_) != VK_SUCCESS) {
        fprintf(stderr, "[Pictor] Failed to create Vulkan instance\n");
        return false;
    }

    // Debug messenger
    if (cfg.validation) {
        VkDebugUtilsMessengerCreateInfoEXT dbg{};
        dbg.sType           = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
        dbg.messageSeverity  = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                               VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        dbg.messageType      = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                               VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                               VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        dbg.pfnUserCallback  = debug_callback;

        auto func = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(instance_, "vkCreateDebugUtilsMessengerEXT"));
        if (func) func(instance_, &dbg, nullptr, &debug_messenger_);
    }

    return true;
}

// ---------- private: surface ----------

bool VulkanContext::create_surface() {
    auto handle = provider_->get_native_handle();

    switch (handle.type) {
#ifdef VK_USE_PLATFORM_WIN32_KHR
    case NativeWindowHandle::Type::WIN32: {
        VkWin32SurfaceCreateInfoKHR info{};
        info.sType     = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
        info.hwnd      = static_cast<HWND>(handle.win32.hwnd);
        info.hinstance = static_cast<HINSTANCE>(handle.win32.hinstance);
        if (vkCreateWin32SurfaceKHR(instance_, &info, nullptr, &surface_) != VK_SUCCESS) {
            fprintf(stderr, "[Pictor] Failed to create Win32 surface\n");
            return false;
        }
        return true;
    }
#endif

#ifdef VK_USE_PLATFORM_XLIB_KHR
    case NativeWindowHandle::Type::XLIB: {
        VkXlibSurfaceCreateInfoKHR info{};
        info.sType  = VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR;
        info.dpy    = static_cast<Display*>(handle.xlib.display);
        info.window = static_cast<Window>(handle.xlib.window);
        if (vkCreateXlibSurfaceKHR(instance_, &info, nullptr, &surface_) != VK_SUCCESS) {
            fprintf(stderr, "[Pictor] Failed to create Xlib surface\n");
            return false;
        }
        return true;
    }
#endif

#ifdef VK_USE_PLATFORM_XCB_KHR
    case NativeWindowHandle::Type::XCB: {
        VkXcbSurfaceCreateInfoKHR info{};
        info.sType      = VK_STRUCTURE_TYPE_XCB_SURFACE_CREATE_INFO_KHR;
        info.connection = static_cast<xcb_connection_t*>(handle.xcb.connection);
        info.window     = static_cast<xcb_window_t>(handle.xcb.window);
        if (vkCreateXcbSurfaceKHR(instance_, &info, nullptr, &surface_) != VK_SUCCESS) {
            fprintf(stderr, "[Pictor] Failed to create XCB surface\n");
            return false;
        }
        return true;
    }
#endif

#ifdef VK_USE_PLATFORM_WAYLAND_KHR
    case NativeWindowHandle::Type::WAYLAND: {
        VkWaylandSurfaceCreateInfoKHR info{};
        info.sType   = VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR;
        info.display = static_cast<wl_display*>(handle.wayland.display);
        info.surface = static_cast<wl_surface*>(handle.wayland.surface);
        if (vkCreateWaylandSurfaceKHR(instance_, &info, nullptr, &surface_) != VK_SUCCESS) {
            fprintf(stderr, "[Pictor] Failed to create Wayland surface\n");
            return false;
        }
        return true;
    }
#endif

#ifdef VK_USE_PLATFORM_MACOS_MVK
    case NativeWindowHandle::Type::COCOA: {
        VkMacOSSurfaceCreateInfoMVK info{};
        info.sType = VK_STRUCTURE_TYPE_MACOS_SURFACE_CREATE_INFO_MVK;
        info.pView = handle.cocoa.ns_view;
        if (vkCreateMacOSSurfaceMVK(instance_, &info, nullptr, &surface_) != VK_SUCCESS) {
            fprintf(stderr, "[Pictor] Failed to create macOS surface\n");
            return false;
        }
        return true;
    }
#endif

#ifdef VK_USE_PLATFORM_ANDROID_KHR
    case NativeWindowHandle::Type::ANDROID: {
        VkAndroidSurfaceCreateInfoKHR info{};
        info.sType  = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR;
        info.window = static_cast<ANativeWindow*>(handle.android.native_window);
        if (vkCreateAndroidSurfaceKHR(instance_, &info, nullptr, &surface_) != VK_SUCCESS) {
            fprintf(stderr, "[Pictor] Failed to create Android surface\n");
            return false;
        }
        return true;
    }
#endif

    default:
        fprintf(stderr, "[Pictor] Unsupported or unavailable platform surface type %d\n",
                static_cast<int>(handle.type));
        return false;
    }
}

// ---------- private: physical device ----------

bool VulkanContext::pick_physical_device() {
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(instance_, &count, nullptr);
    if (count == 0) {
        fprintf(stderr, "[Pictor] No Vulkan-capable GPU found\n");
        return false;
    }

    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(instance_, &count, devices.data());

    // Pick first device that has a graphics queue supporting present
    for (auto& dev : devices) {
        uint32_t qf_count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(dev, &qf_count, nullptr);
        std::vector<VkQueueFamilyProperties> qf_props(qf_count);
        vkGetPhysicalDeviceQueueFamilyProperties(dev, &qf_count, qf_props.data());

        for (uint32_t i = 0; i < qf_count; ++i) {
            VkBool32 present_support = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(dev, i, surface_, &present_support);

            if ((qf_props[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present_support) {
                physical_device_    = dev;
                queue_family_index_ = i;

                VkPhysicalDeviceProperties props;
                vkGetPhysicalDeviceProperties(dev, &props);
                printf("[Pictor] Using GPU: %s\n", props.deviceName);
                return true;
            }
        }
    }

    fprintf(stderr, "[Pictor] No suitable GPU found (need graphics + present)\n");
    return false;
}

// ---------- private: logical device ----------

bool VulkanContext::create_logical_device() {
    float priority = 1.0f;
    VkDeviceQueueCreateInfo queue_info{};
    queue_info.sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queue_info.queueFamilyIndex = queue_family_index_;
    queue_info.queueCount       = 1;
    queue_info.pQueuePriorities = &priority;

    VkPhysicalDeviceFeatures features{};

    const char* dev_extensions[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };

    VkDeviceCreateInfo create_info{};
    create_info.sType                   = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    create_info.queueCreateInfoCount    = 1;
    create_info.pQueueCreateInfos       = &queue_info;
    create_info.pEnabledFeatures        = &features;
    create_info.enabledExtensionCount   = 1;
    create_info.ppEnabledExtensionNames = dev_extensions;

    if (vkCreateDevice(physical_device_, &create_info, nullptr, &device_) != VK_SUCCESS) {
        fprintf(stderr, "[Pictor] Failed to create Vulkan logical device\n");
        return false;
    }

    vkGetDeviceQueue(device_, queue_family_index_, 0, &graphics_queue_);
    present_queue_ = graphics_queue_;
    return true;
}

// ---------- private: swapchain ----------

bool VulkanContext::create_swapchain() {
    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical_device_, surface_, &caps);

    uint32_t fmt_count = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device_, surface_, &fmt_count, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(fmt_count);
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device_, surface_, &fmt_count, formats.data());

    uint32_t pm_count = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(physical_device_, surface_, &pm_count, nullptr);
    std::vector<VkPresentModeKHR> present_modes(pm_count);
    vkGetPhysicalDeviceSurfacePresentModesKHR(physical_device_, surface_, &pm_count, present_modes.data());

    // Choose format
    swapchain_format_ = formats[0].format;
    VkColorSpaceKHR color_space = formats[0].colorSpace;
    for (auto& f : formats) {
        if (f.format == VK_FORMAT_B8G8R8A8_SRGB &&
            f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            swapchain_format_ = f.format;
            color_space = f.colorSpace;
            break;
        }
    }

    // Choose present mode
    auto sc_config = provider_->get_swapchain_config();
    VkPresentModeKHR present_mode = VK_PRESENT_MODE_FIFO_KHR; // guaranteed
    if (!sc_config.vsync) {
        for (auto m : present_modes) {
            if (m == VK_PRESENT_MODE_MAILBOX_KHR) { present_mode = m; break; }
        }
    }

    // Choose extent
    if (caps.currentExtent.width != UINT32_MAX) {
        swapchain_extent_ = caps.currentExtent;
    } else {
        swapchain_extent_.width  = std::clamp(sc_config.width,
            caps.minImageExtent.width, caps.maxImageExtent.width);
        swapchain_extent_.height = std::clamp(sc_config.height,
            caps.minImageExtent.height, caps.maxImageExtent.height);
    }

    uint32_t image_count = std::max(sc_config.image_count, caps.minImageCount);
    if (caps.maxImageCount > 0) image_count = std::min(image_count, caps.maxImageCount);

    VkSwapchainCreateInfoKHR sc_info{};
    sc_info.sType            = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    sc_info.surface          = surface_;
    sc_info.minImageCount    = image_count;
    sc_info.imageFormat      = swapchain_format_;
    sc_info.imageColorSpace  = color_space;
    sc_info.imageExtent      = swapchain_extent_;
    sc_info.imageArrayLayers = 1;
    sc_info.imageUsage       = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    sc_info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    sc_info.preTransform     = caps.currentTransform;
    sc_info.compositeAlpha   = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    sc_info.presentMode      = present_mode;
    sc_info.clipped          = VK_TRUE;
    sc_info.oldSwapchain     = VK_NULL_HANDLE;

    if (vkCreateSwapchainKHR(device_, &sc_info, nullptr, &swapchain_) != VK_SUCCESS) {
        fprintf(stderr, "[Pictor] Failed to create swapchain\n");
        return false;
    }

    vkGetSwapchainImagesKHR(device_, swapchain_, &image_count, nullptr);
    swapchain_images_.resize(image_count);
    vkGetSwapchainImagesKHR(device_, swapchain_, &image_count, swapchain_images_.data());

    provider_->on_swapchain_created(swapchain_extent_.width, swapchain_extent_.height);
    return true;
}

// ---------- private: image views ----------

bool VulkanContext::create_image_views() {
    swapchain_image_views_.resize(swapchain_images_.size());
    for (size_t i = 0; i < swapchain_images_.size(); ++i) {
        VkImageViewCreateInfo info{};
        info.sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        info.image    = swapchain_images_[i];
        info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        info.format   = swapchain_format_;
        info.components = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                           VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};
        info.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        info.subresourceRange.baseMipLevel   = 0;
        info.subresourceRange.levelCount     = 1;
        info.subresourceRange.baseArrayLayer = 0;
        info.subresourceRange.layerCount     = 1;

        if (vkCreateImageView(device_, &info, nullptr, &swapchain_image_views_[i]) != VK_SUCCESS) {
            fprintf(stderr, "[Pictor] Failed to create image view %zu\n", i);
            return false;
        }
    }
    return true;
}

// ---------- private: render pass ----------

bool VulkanContext::create_render_pass() {
    VkAttachmentDescription color_attachment{};
    color_attachment.format         = swapchain_format_;
    color_attachment.samples        = VK_SAMPLE_COUNT_1_BIT;
    color_attachment.loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color_attachment.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
    color_attachment.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color_attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color_attachment.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
    color_attachment.finalLayout    = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VkAttachmentReference color_ref{};
    color_ref.attachment = 0;
    color_ref.layout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint    = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments    = &color_ref;

    VkSubpassDependency dependency{};
    dependency.srcSubpass    = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass    = 0;
    dependency.srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.srcAccessMask = 0;
    dependency.dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo info{};
    info.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    info.attachmentCount = 1;
    info.pAttachments    = &color_attachment;
    info.subpassCount    = 1;
    info.pSubpasses      = &subpass;
    info.dependencyCount = 1;
    info.pDependencies   = &dependency;

    if (vkCreateRenderPass(device_, &info, nullptr, &render_pass_) != VK_SUCCESS) {
        fprintf(stderr, "[Pictor] Failed to create render pass\n");
        return false;
    }
    return true;
}

// ---------- private: framebuffers ----------

bool VulkanContext::create_framebuffers() {
    framebuffers_.resize(swapchain_image_views_.size());
    for (size_t i = 0; i < swapchain_image_views_.size(); ++i) {
        VkFramebufferCreateInfo info{};
        info.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        info.renderPass      = render_pass_;
        info.attachmentCount = 1;
        info.pAttachments    = &swapchain_image_views_[i];
        info.width           = swapchain_extent_.width;
        info.height          = swapchain_extent_.height;
        info.layers          = 1;

        if (vkCreateFramebuffer(device_, &info, nullptr, &framebuffers_[i]) != VK_SUCCESS) {
            fprintf(stderr, "[Pictor] Failed to create framebuffer %zu\n", i);
            return false;
        }
    }
    return true;
}

// ---------- private: command pool ----------

bool VulkanContext::create_command_pool_and_buffers() {
    VkCommandPoolCreateInfo pool_info{};
    pool_info.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool_info.queueFamilyIndex = queue_family_index_;

    if (vkCreateCommandPool(device_, &pool_info, nullptr, &command_pool_) != VK_SUCCESS) {
        fprintf(stderr, "[Pictor] Failed to create command pool\n");
        return false;
    }

    command_buffers_.resize(swapchain_images_.size());
    VkCommandBufferAllocateInfo alloc_info{};
    alloc_info.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    alloc_info.commandPool        = command_pool_;
    alloc_info.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc_info.commandBufferCount = static_cast<uint32_t>(command_buffers_.size());

    if (vkAllocateCommandBuffers(device_, &alloc_info, command_buffers_.data()) != VK_SUCCESS) {
        fprintf(stderr, "[Pictor] Failed to allocate command buffers\n");
        return false;
    }
    return true;
}

// ---------- private: sync objects ----------

bool VulkanContext::create_sync_objects() {
    VkSemaphoreCreateInfo sem_info{};
    sem_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

    VkFenceCreateInfo fence_info{};
    fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fence_info.flags = VK_FENCE_CREATE_SIGNALED_BIT;

    if (vkCreateSemaphore(device_, &sem_info, nullptr, &image_available_sem_) != VK_SUCCESS ||
        vkCreateSemaphore(device_, &sem_info, nullptr, &render_finished_sem_) != VK_SUCCESS ||
        vkCreateFence(device_, &fence_info, nullptr, &in_flight_fence_) != VK_SUCCESS) {
        fprintf(stderr, "[Pictor] Failed to create sync objects\n");
        return false;
    }
    return true;
}

// ---------- private: cleanup ----------

void VulkanContext::cleanup_swapchain() {
    for (auto fb : framebuffers_) {
        if (fb) vkDestroyFramebuffer(device_, fb, nullptr);
    }
    framebuffers_.clear();

    if (render_pass_) {
        vkDestroyRenderPass(device_, render_pass_, nullptr);
        render_pass_ = VK_NULL_HANDLE;
    }

    for (auto iv : swapchain_image_views_) {
        if (iv) vkDestroyImageView(device_, iv, nullptr);
    }
    swapchain_image_views_.clear();
    swapchain_images_.clear();

    if (swapchain_) {
        vkDestroySwapchainKHR(device_, swapchain_, nullptr);
        swapchain_ = VK_NULL_HANDLE;
    }
}

} // namespace pictor

#else // !PICTOR_HAS_VULKAN

namespace pictor {

VulkanContext::VulkanContext() = default;
VulkanContext::~VulkanContext() = default;

bool VulkanContext::initialize(ISurfaceProvider*, const VulkanContextConfig&) {
    fprintf(stderr, "[Pictor] Vulkan not available (PICTOR_HAS_VULKAN not defined)\n");
    return false;
}

void VulkanContext::shutdown() {}
bool VulkanContext::recreate_swapchain() { return false; }
uint32_t VulkanContext::acquire_next_image() { return UINT32_MAX; }
bool VulkanContext::present(uint32_t) { return false; }
void VulkanContext::device_wait_idle() {}

} // namespace pictor

#endif // PICTOR_HAS_VULKAN
