#pragma once
#include "pn_gpu_data.h"
#include <vulkan/vulkan.h>
#include <string>

namespace pictor_kuzuha {
struct alignas(16) RaymarchParameters {
    float viewport[4]{}; // width, height, alpha, display
    float right[4]{}, up[4]{}, forward[4]{};
    float quality[4]{}; // angular epsilon, fixed epsilon (0=auto), FP margin, spare
};
static_assert(sizeof(RaymarchParameters)==80);

class RaymarchPass {
public:
    struct CreateInfo {
        VkDevice device{};
        VkPhysicalDevice physical_device{};
        VkRenderPass render_pass{};
        VkDescriptorSetLayout scene_layout{}, texture_layout{};
        std::string shader_dir;
    };
    bool create(const CreateInfo& info,const std::vector<GpuPnPatch>& patches);
    void destroy();
    // Caller has waited for the preceding frame's fence before accessing counters.
    uint32_t begin_frame();
    // Sparse host updates are allowed only after the preceding frame fence.
    void update_patch(uint32_t index,const GpuPnPatch& patch);
    void finish_frame(VkCommandBuffer cmd) const;
    void bind(VkCommandBuffer cmd,VkDescriptorSet scene,const RaymarchParameters& parameters) const;
    void draw(VkCommandBuffer cmd,VkDescriptorSet texture,uint32_t first_patch,uint32_t count) const;
private:
    VkDevice device_{};
    VkPipeline pipeline_{};
    VkPipelineLayout layout_{};
    VkDescriptorSetLayout patch_layout_{};
    VkDescriptorPool pool_{};
    VkDescriptorSet set_{};
    VkBuffer buffer_{}, counters_{};
    VkDeviceMemory memory_{}, counters_memory_{};
    uint32_t* mapped_counters_{};
    GpuPnPatch* mapped_patches_{};
    size_t patch_count_=0;
};
}
