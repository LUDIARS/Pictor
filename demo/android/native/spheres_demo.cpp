#include "demo_pipeline.h"
#include "pictor/surface/simple_renderer.h"
#include <stdexcept>

namespace {
class Spheres final : public DemoPipeline {
    pictor::SimpleRenderer renderer_;
public:
    Spheres(pictor::VulkanContext& context, const char* shaders) {
        if (!renderer_.initialize(context, shaders)) throw std::runtime_error("Instancing pipeline initialization failed");
        const float instances[] = {-0.55f, 0, 0.5f, 0.22f, 0, 0, 0.5f, 0.22f, 0.55f, 0, 0.5f, 0.22f};
        renderer_.update_instances(instances, 3);
    }
    void record(pictor::VulkanContext& context, uint32_t image) override {
        const float view[] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
        float projection[] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
        const auto extent = context.swapchain_extent();
        projection[0] = static_cast<float>(extent.height) / extent.width;
        renderer_.render(context.command_buffers()[image], context.default_render_pass(),
                         context.framebuffers()[image], extent, view, projection);
    }
};
}
std::unique_ptr<DemoPipeline> make_spheres(pictor::VulkanContext& context, const char* shaders) {
    return std::make_unique<Spheres>(context, shaders);
}
