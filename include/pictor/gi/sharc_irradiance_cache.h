#pragma once

#ifndef PICTOR_HAS_SHARC_VULKAN
#error "SHaRC Vulkan requires linking Pictor::sharc_vulkan (PICTOR_ENABLE_SHARC_VULKAN=ON)"
#endif

/// SharcIrradianceCache — the SHaRC module's path for hosts that light their
/// own G-buffer (spec: spec/feature/optional-sharc-module.md, "irradiance cache").
///
/// Every frame, after the host has drawn its G-buffer, record() runs three
/// compute passes on the host's command buffer:
///   1. update  — one ray query per update_block^2 pixels from the G-buffer
///                into the host's geometry; what it brings back (emission,
///                direct light, ambient and the cache itself at the hit)
///                is added to the pixel's cell,
///   2. resolve — each cell folds the frame's samples into its running mean
///                and evicts itself when unsampled for long,
///   3. query   — every pixel looks its cell up and writes the diffuse
///                irradiance to irradiance_view() (rgba16f, a = found).
/// The host reflects that irradiance with its own surface colours.
///
/// Inputs are borrowed: the geometry and its acceleration structures
/// (SharcSceneAcceleration), the surface and light tables, the sun's shadow
/// map, and the G-buffer's depth (Vulkan clip depth 0..1) and world normal.
/// The sampled images are read in the layouts given; the host keeps them
/// alive and calls set_* again when it recreates them. The context must have
/// been created with VulkanConfig::require_ray_query.
///
/// SRP: Vulkan resources and dispatch recording of the irradiance passes.
/// Cell semantics live in shaders/sharc/sharc_irradiance.glsl and
/// sharc_common.glsl.

#include "pictor/gi/sharc_irradiance_types.h"

#include <cstdint>
#include <string>
#include <vector>

#ifdef PICTOR_HAS_VULKAN
#include <vulkan/vulkan.h>
#endif

namespace pictor {

class VulkanContext;

#ifdef PICTOR_HAS_VULKAN
/// The traced geometry: acceleration structures and what a hit reads.
struct SharcIrradianceScene {
    VkAccelerationStructureKHR top_level = VK_NULL_HANDLE;
    VkBuffer instances = VK_NULL_HANDLE;  ///< SharcRayInstanceGpu[]
    VkDeviceSize instances_bytes = 0;
    VkBuffer vertices = VK_NULL_HANDLE;   ///< read as float[]
    VkDeviceSize vertices_bytes = 0;
    VkBuffer indices = VK_NULL_HANDLE;    ///< uint32[]
    VkDeviceSize indices_bytes = 0;
    VkBuffer surfaces = VK_NULL_HANDLE;   ///< SharcRaySurfaceGpu[]
    VkDeviceSize surfaces_bytes = 0;
    uint32_t vertex_stride_floats = 0;
    uint32_t position_offset_floats = 0;
    uint32_t normal_offset_floats = 3;
};
#endif

class SharcIrradianceCache {
public:
    SharcIrradianceCache() = default;
    ~SharcIrradianceCache();
    SharcIrradianceCache(const SharcIrradianceCache&) = delete;
    SharcIrradianceCache& operator=(const SharcIrradianceCache&) = delete;

#ifdef PICTOR_HAS_VULKAN
    /// `shader_dir` holds sharc_irradiance_{update,resolve,query}.comp.spv.
    bool initialize(VulkanContext& vk, const std::string& shader_dir, const SharcIrradianceConfig& config);

    void set_scene(const SharcIrradianceScene& scene);
    /// SharcIrradianceLightGpu[] (the frame's counts.x says how many are live).
    void set_lights(VkBuffer lights, VkDeviceSize bytes);
    /// The sun's shadow map (depth), sampled in `layout` with a nearest sampler.
    void set_sun_shadow(VkImageView shadow, VkImageLayout layout);
    /// The G-buffer the passes read (depth in `depth_layout`, world normal rgb in
    /// SHADER_READ_ONLY_OPTIMAL); (re)creates the irradiance image at `extent`.
    bool set_gbuffer(VkImageView depth, VkImageLayout depth_layout, VkImageView normal, VkExtent2D extent);

    /// True once the scene, lights, sun shadow and G-buffer are all set.
    bool ready() const;

    /// Records the three passes for this frame (`frame` written to this frame's
    /// slot; its screen must be the G-buffer's extent). Leaves irradiance_view()
    /// in SHADER_READ_ONLY_OPTIMAL for fragment shaders. False when not ready.
    bool record(VkCommandBuffer cmd, const SharcIrradianceFrameGpu& frame);

    VkImageView irradiance_view() const { return output_view_; }
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

    bool create_buffer_(VkDeviceSize size, VkBufferUsageFlags usage, bool host_visible, Buffer& out);
    void destroy_buffer_(Buffer& b);
    bool create_pipeline_(const std::string& path, VkPipeline& out);
    bool create_output_(VkExtent2D extent);
    void destroy_output_();
    void write_buffer_(uint32_t binding, VkDescriptorType type, VkBuffer buffer, VkDeviceSize offset, VkDeviceSize range);
    void write_image_(uint32_t binding, VkDescriptorType type, VkImageView view, VkImageLayout layout);
    void compute_barrier_(VkCommandBuffer cmd) const;

    VulkanContext* vk_ = nullptr;
    VkDevice device_ = VK_NULL_HANDLE;
    SharcIrradianceConfig config_{};

    Buffer params_;  // SharcParamsGpu per frame in flight (dynamic)
    Buffer frame_;   // SharcIrradianceFrameGpu per frame in flight (dynamic)
    VkDeviceSize params_stride_ = 0, frame_stride_ = 0;
    uint32_t slots_ = 0;
    Buffer keys_, cells_, accum_;

    VkImage output_ = VK_NULL_HANDLE;
    VkDeviceMemory output_memory_ = VK_NULL_HANDLE;
    VkImageView output_view_ = VK_NULL_HANDLE;
    VkExtent2D output_extent_{};
    bool output_fresh_ = true;  // never written: its first barrier starts from UNDEFINED

    VkSampler nearest_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    VkDescriptorSet set_ = VK_NULL_HANDLE;
    VkPipeline update_ = VK_NULL_HANDLE, resolve_ = VK_NULL_HANDLE, query_ = VK_NULL_HANDLE;

    uint32_t vertex_layout_[4] = {0, 0, 0, 0};
    bool has_scene_ = false, has_lights_ = false, has_shadow_ = false, has_gbuffer_ = false;
    uint32_t frame_counter_ = 0;
#endif
    bool initialized_ = false;
};

}  // namespace pictor
