// Toon tears for the FBX viewer.
//
// Each eye anchor emits teardrops that slide down the body. Drops are
// camera-facing quads; tear.frag draws a flat-shaded teardrop with a dark
// outline and a specular dot (manga style). Geometry is rebuilt on the
// CPU every frame into a host-visible buffer (a handful of quads).
#pragma once

#include "pictor/core/types.h"

#include <vulkan/vulkan.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace pictor_fbx_viewer {

struct TearVertex {
    float position[3];   // world
    float corner[2];     // quad corner in [-1, 1]
    float age;           // 0..1 along the drop's life
    float pad;
};
static_assert(sizeof(TearVertex) == 28, "TearVertex must match tear.vert");

struct TearEmitter {
    pictor::float3 position{};   // skinned eye position (world)
    pictor::float3 normal{};     // skinned surface normal at the eye
    pictor::float3 outward{};    // tangent-plane direction away from the face centre
};

/// Maps a world point back onto the body surface so drops hug a curved
/// face instead of falling into it. Returns the surface point and normal.
using SurfaceProjectFn = std::function<bool(const pictor::float3& p,
                                            pictor::float3& on_surface,
                                            pictor::float3& normal)>;

class TearPass {
public:
    struct CreateInfo {
        VkDevice              device = VK_NULL_HANDLE;
        VkPhysicalDevice      physical_device = VK_NULL_HANDLE;
        VkRenderPass          render_pass = VK_NULL_HANDLE;
        VkDescriptorSetLayout scene_layout = VK_NULL_HANDLE;
        std::string           shader_dir;
    };
    /// Manga "spraying" tears: a fan of drops beside each eye, tips pointing
    /// back at the eye, trembling in place.
    struct Params {
        float    drop_size     = 1.0f;   // half-width of a drop, model units
        float    surface_lift  = 0.4f;   // offset along the normal (above the fur)
        // Drops travel in a single line from the outer corner of the eye,
        // outward and downward, like beads on a string; each slot streams
        // away from the eye and wraps (a continuous trickle).
        float    line_start    = 2.0f;   // eye -> first drop distance
        float    line_length   = 5.0f;   // distance covered by the line
        float    line_tilt_deg = -28.0f; // negative = downward from the outward axis
        float    flow_hz       = 0.35f;  // how many times per second a drop runs the line
        float    tremble_hz    = 9.0f;   // in-place jitter frequency
        float    tremble_amp   = 0.06f;  // jitter amplitude (fraction of drop size)
        uint32_t drops_per_eye = 3;
    };

    bool create(const CreateInfo& ci);
    void destroy();

    /// Rebuild the drop quads for this frame.
    void update(float time_s,
                const std::vector<TearEmitter>& emitters,
                const pictor::float3& camera_pos,
                const SurfaceProjectFn& project,
                const Params& params);

    void record(VkCommandBuffer cmd, VkDescriptorSet scene_set) const;
    bool valid() const { return pipeline_ != VK_NULL_HANDLE; }

private:
    bool ensure_capacity(uint32_t vertex_count);

    VkDevice         device_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device_ = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;
    VkPipeline       pipeline_ = VK_NULL_HANDLE;
    VkBuffer         vb_ = VK_NULL_HANDLE; VkDeviceMemory vb_mem_ = VK_NULL_HANDLE;
    uint32_t         capacity_ = 0;
    uint32_t         vertex_count_ = 0;
};

} // namespace pictor_fbx_viewer
