#pragma once

#include "pictor/surface/frame_result.h"
#include <vulkan/vulkan.h>

namespace pictor {

inline FrameStatus vulkan_frame_status(VkResult result) {
    switch (result) {
    case VK_SUCCESS: return FrameStatus::Ready;
    case VK_SUBOPTIMAL_KHR:
    case VK_ERROR_OUT_OF_DATE_KHR: return FrameStatus::RecreateSwapchain;
    case VK_ERROR_SURFACE_LOST_KHR: return FrameStatus::SurfaceLost;
    case VK_ERROR_DEVICE_LOST: return FrameStatus::DeviceLost;
    default: return FrameStatus::Error;
    }
}

} // namespace pictor
