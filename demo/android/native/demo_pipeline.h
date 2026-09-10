#pragma once
#include "pictor/surface/vulkan_context.h"
#include <memory>

// Only the host lifecycle is shared; each demo owns its rendering implementation.
class DemoPipeline {
public:
    virtual ~DemoPipeline() = default;
    virtual void record(pictor::VulkanContext& context, uint32_t image) = 0;
};
std::unique_ptr<DemoPipeline> make_spheres(pictor::VulkanContext& context, const char* shaders);
std::unique_ptr<DemoPipeline> make_surface_demo();
