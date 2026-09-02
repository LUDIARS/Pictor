// Shell-fur pass for the FBX viewer.
//
// Implements a shell-fur look (shell extrusion along the skinned normal,
// procedural strand mask, root occlusion, tip tint,
// soft rim + back-scatter) onto Pictor's Vulkan skinning pipeline.
//
// The base mesh is drawn by the viewer's regular pipeline; this pass then
// re-draws every submesh `shell_count` times with a per-shell push
// constant (shell index / count + material knobs). It shares set 0
// (scene UBO + instances + bones) and set 1 (diffuse texture) with the
// base pipeline so no extra descriptors are needed.
#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>
#include <string>
#include <vector>

namespace pictor_fbx_viewer {

/// Tunable fur look. Lengths are in model units (the FBX's own scale),
/// so callers normally derive `length` / `bend` from the mesh radius.
struct FurShellParams {
    // Defaults tuned to a short-pile minky plush (dense sub-pixel nap, soft
    // velvet sheen) rather than a long visible-strand preset.
    uint32_t shell_count    = 16;
    float    length         = 1.0f;     // extrusion at the outermost shell
    float    density        = 220.0f;   // strands per UV unit
    float    root_thickness = 0.58f;    // strand radius at the root (cell units)
    float    tip_thickness  = 0.20f;    // strand radius at the tip
    float    root_occlusion = 0.22f;    // darkening at the root
    float    bend[3]        = {0.0f, -0.1f, 0.0f};    // gravity droop, model units
    float    tip_color[3]   = {1.0f, 0.985f, 0.96f};  // warm white tips
    float    rim_color[3]   = {1.0f, 0.97f, 0.93f};   // velvet sheen, not purple
    float    rim_power      = 2.2f;
    float    ground_bounce  = 0.10f;    // pseudo-GI floor bounce strength

    void print() const;
};

/// Push-constant block. Must match fur_shell.vert / fur_shell.frag.
struct FurShellPush {
    float tip_color[4];   // rgb, w unused
    float rim_color[4];   // rgb, w = rim power
    float bend[4];        // xyz bend, w = fur length
    float shape[4];       // density, root thickness, tip thickness, root occlusion
    float shell[4];       // shell index, shell count, ground bounce, unused
};
static_assert(sizeof(FurShellPush) == 80, "FurShellPush must stay <= 128 bytes");

struct FurShellSubmeshDraw {
    uint32_t        index_start = 0;
    uint32_t        index_count = 0;
    VkDescriptorSet texture_set = VK_NULL_HANDLE;
};

class FurShellPass {
public:
    struct CreateInfo {
        VkDevice              device       = VK_NULL_HANDLE;
        VkRenderPass          render_pass  = VK_NULL_HANDLE;
        VkDescriptorSetLayout scene_layout = VK_NULL_HANDLE;   // set 0
        VkDescriptorSetLayout tex_layout   = VK_NULL_HANDLE;   // set 1
        std::string           shader_dir;
    };

    bool create(const CreateInfo& ci);
    void destroy();

    /// Records the shell draws. Caller has already bound the vertex/index
    /// buffers (same buffers as the base pass) and set viewport/scissor.
    void record(VkCommandBuffer cmd,
                VkDescriptorSet scene_set,
                const std::vector<FurShellSubmeshDraw>& submeshes,
                const FurShellParams& params) const;

    bool valid() const { return pipeline_ != VK_NULL_HANDLE; }

private:
    VkDevice         device_          = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;
    VkPipeline       pipeline_        = VK_NULL_HANDLE;
};

} // namespace pictor_fbx_viewer
