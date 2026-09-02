// Rope geometry pass for the FBX viewer.
//
// The deformation uses capsule uniforms; the viewer also draws the rope as
// a tube swept along each strand polyline so the tie can be read on screen.
// Static geometry in world space, lit by the scene UBO (set 0 only).
#pragma once

#include "nawa_binding.h"

#include <vulkan/vulkan.h>

#include <cstdint>
#include <string>

namespace pictor_fbx_viewer {

struct RopeVertex {
    float position[3];
    float normal[3];
    float uv[2];        // x along the rope (twist stripes), y around it
};
static_assert(sizeof(RopeVertex) == 32, "RopeVertex must match rope.vert");

class RopePass {
public:
    struct CreateInfo {
        VkDevice              device       = VK_NULL_HANDLE;
        VkPhysicalDevice      physical_device = VK_NULL_HANDLE;
        VkRenderPass          render_pass  = VK_NULL_HANDLE;
        VkDescriptorSetLayout scene_layout = VK_NULL_HANDLE;
        std::string           shader_dir;
    };

    bool create(const CreateInfo& ci);
    void destroy();

    /// (Re)build the tube mesh from the chain strands. Safe to call while
    /// idle (waits for the device before replacing buffers).
    bool build_geometry(const NawaCapsuleChain& chain, float rope_radius,
                        uint32_t visible_segments);

    /// Per-frame loose end of the rope (e.g. trailing toward the camera).
    /// Rebuilt into a host-visible buffer each call; an empty polyline
    /// hides it.
    bool update_tail(const std::vector<pictor::float3>& polyline, float rope_radius);

    void record(VkCommandBuffer cmd, VkDescriptorSet scene_set) const;
    bool valid() const { return pipeline_ != VK_NULL_HANDLE; }

private:
    struct DynMesh {
        VkBuffer vb = VK_NULL_HANDLE; VkDeviceMemory vb_mem = VK_NULL_HANDLE;
        VkBuffer ib = VK_NULL_HANDLE; VkDeviceMemory ib_mem = VK_NULL_HANDLE;
        uint32_t vb_capacity = 0, ib_capacity = 0;
    };
    bool upload(DynMesh& mesh, const std::vector<RopeVertex>& verts,
                const std::vector<uint32_t>& idx, bool host_visible_resize);
    void destroy_mesh(DynMesh& mesh);
    void destroy_buffers();

    VkDevice         device_          = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device_ = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;
    VkPipeline       pipeline_        = VK_NULL_HANDLE;
    VkBuffer         vb_ = VK_NULL_HANDLE; VkDeviceMemory vb_mem_ = VK_NULL_HANDLE;
    VkBuffer         ib_ = VK_NULL_HANDLE; VkDeviceMemory ib_mem_ = VK_NULL_HANDLE;
    uint32_t         index_count_ = 0;
    DynMesh          tail_;
    uint32_t         tail_index_count_ = 0;
};

} // namespace pictor_fbx_viewer
