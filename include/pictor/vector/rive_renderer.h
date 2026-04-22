#pragma once

#ifdef PICTOR_HAS_VULKAN
#include <vulkan/vulkan.h>
#endif

#include <cstdint>
#include <memory>
#include <string>

namespace pictor {

class VulkanContext;

/// Pictor wrapper around the Rive Renderer
/// (https://github.com/rive-app/rive-runtime, MIT-licensed).
///
/// Owns a single `rive::gpu::RenderContextVulkanImpl` configured against
/// Pictor's shared `VulkanContext`. A loaded `.riv` file yields an artboard
/// and an optional state machine / animation that is advanced and drawn each
/// frame into a caller-supplied VkImage.
///
/// The wrapper exposes no Rive types in its public header — the Rive include
/// tree only needs to be visible while compiling `rive_renderer.cpp`. This
/// keeps Pictor's public headers free of Rive's Vulkan include chain.
///
/// Compilation model:
///   - When PICTOR_HAS_RIVE is defined (set by CMake when `PICTOR_ENABLE_RIVE=ON`
///     and a prebuilt rive-runtime is located via `cmake/FindRive.cmake`), the
///     implementation forwards to Rive.
///   - When it is not defined, every public method is a safe no-op that
///     reports failure on `initialize()`. This lets downstream code depend on
///     Pictor unconditionally.
class RiveRenderer {
public:
    struct Options {
        /// Force atomic-based fill mode. Required on drivers without rasterizer
        /// ordering support (`VK_EXT_rasterization_order_attachment_access` or
        /// the Rive-preferred pixel-local-storage equivalents). Slightly slower
        /// on compliant drivers but portable.
        bool     force_atomic_mode = false;

        /// MSAA sample count for the Rive output. 0 = no MSAA (default).
        uint32_t msaa_sample_count = 0;

        /// Packed 0xAABBGGRR clear color applied each frame.
        uint32_t clear_color = 0x00000000;
    };

    RiveRenderer();
    ~RiveRenderer();

    RiveRenderer(const RiveRenderer&)            = delete;
    RiveRenderer& operator=(const RiveRenderer&) = delete;

    // ─── Lifecycle ───────────────────────────────────────────

    /// Create the Rive RenderContext bound to the given Vulkan context.
    /// Returns false if Rive is not compiled in (PICTOR_HAS_RIVE undefined)
    /// or if Rive initialisation fails.
    bool initialize(VulkanContext& vk_ctx, const Options& opts = {});

    void shutdown();

    bool is_initialized() const;

    // ─── Asset loading ───────────────────────────────────────

    /// Load a .riv file from disk. Replaces any previously-loaded file.
    bool load_riv_file(const std::string& path);

    /// Load a .riv file from an in-memory buffer. `data` is copied; the
    /// caller may free the buffer immediately after the call returns.
    bool load_riv_memory(const uint8_t* data, size_t size);

    void unload();

    bool has_file() const;

    // ─── Scene selection ─────────────────────────────────────

    /// Pick an artboard by index. -1 selects the default artboard.
    bool set_artboard(int index = -1);

    /// Pick a state machine. Use `set_state_machine(-1)` to disable and
    /// fall back to an animation.
    bool set_state_machine(int index);

    /// Pick an animation (ignored if a state machine is active).
    bool set_animation(int index);

    bool has_scene() const;

    // ─── Per-frame ───────────────────────────────────────────

    /// Advance the currently selected scene by `delta_time` seconds.
    void advance(float delta_time);

#ifdef PICTOR_HAS_VULKAN
    /// Draw the current scene into the supplied Vulkan image.
    ///
    /// Layout tracking is handled internally: the wrapper remembers the
    /// last access state per VkImage it has seen, so the caller never
    /// needs to tell Rive "what layout is this image in". First-time images
    /// are assumed to be in VK_IMAGE_LAYOUT_UNDEFINED (the content is
    /// discarded — fine for swapchain images immediately after acquire).
    ///
    /// `final_layout` optionally requests a final layout transition after
    /// Rive's flush. Passing `VK_IMAGE_LAYOUT_PRESENT_SRC_KHR` handles the
    /// common swapchain case — the wrapper queries the layout Rive actually
    /// ended in (which depends on atomic vs raster-ordered mode, MSAA, and
    /// the Rive version) and emits a single barrier from that to
    /// `final_layout`. Passing VK_IMAGE_LAYOUT_UNDEFINED (the default)
    /// skips the transition.
    ///
    /// `current_frame_number` / `safe_frame_number` feed Rive's internal
    /// ring-buffered GPU resource recycling — the caller typically passes
    /// the active swapchain frame and the last frame known to be retired.
    bool render(VkCommandBuffer  cmd,
                VkImage          target_image,
                VkImageView      target_view,
                VkExtent2D       extent,
                VkFormat         format,
                uint32_t         current_frame_number,
                uint32_t         safe_frame_number,
                VkImageLayout    final_layout = VK_IMAGE_LAYOUT_UNDEFINED);
#endif

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace pictor
