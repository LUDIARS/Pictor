#include "demo_pipeline.h"
#include <cmath>

namespace {
class SurfaceDemo final : public DemoPipeline {
    uint32_t frame_ = 0;
public:
    void record(pictor::VulkanContext& context, uint32_t image) override {
        VkClearValue color{};
        color.color = {{0.05f, 0.2f, 0.4f + 0.3f * std::sin(++frame_ * 0.025f), 1.0f}};
        VkRenderPassBeginInfo pass{};
        pass.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        pass.renderPass = context.default_render_pass();
        pass.framebuffer = context.framebuffers()[image];
        pass.renderArea.extent = context.swapchain_extent();
        pass.clearValueCount = 1;
        pass.pClearValues = &color;
        const auto command = context.command_buffers()[image];
        vkCmdBeginRenderPass(command, &pass, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdEndRenderPass(command);
    }
};
}
std::unique_ptr<DemoPipeline> make_surface_demo() { return std::make_unique<SurfaceDemo>(); }
