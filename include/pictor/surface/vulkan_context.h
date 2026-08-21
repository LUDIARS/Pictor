#pragma once

#include "pictor/surface/surface_provider.h"
#include "pictor/surface/per_image_resource_pool.h"
#include "pictor/surface/vulkan_per_image_backend.h"
#include "pictor/core/device_memory_profile.h"
#include <cstdint>
#include <vector>
#include <string>
#include <functional>

#ifdef PICTOR_HAS_VULKAN
#include <vulkan/vulkan.h>
#endif

namespace pictor {

/// Configuration for Vulkan instance / device creation.
struct VulkanContextConfig {
    const char* app_name    = "Pictor";
    uint32_t    app_version = 1;
    bool        validation  = false;  // VK_LAYER_KHRONOS_validation
    /// 既定: true — VulkanContext が組み込みの swapchain default render pass
    /// + framebuffers を自動生成する (simple_renderer / ui_overlay_pipeline /
    /// bitmap_text_renderer 等の easy-mode consumer 向け)。
    ///
    /// false にすると create_render_pass / framebuffers を skip し、 host が
    /// RenderPassRegistry + FramebufferRegistry 経由で profile-driven に作る
    /// 前提になる (KS Phase 4 step 4 など `default_render_pass()` を使わない
    /// パス専用)。 default_render_pass() / framebuffers() は VK_NULL_HANDLE を
    /// 返すので、 これらに依存するコンポーネントは同居できない。
    bool        create_default_render_pass = true;

    /// CPU が GPU に先行できる同時フレーム数 (frames-in-flight、 Q-3)。
    /// 1 = 完全直列 (旧挙動、 acquire ごとに前フレームの GPU 完了を待つ)。
    /// 2 以上で CPU 側の記録と GPU 実行を重ねられる。 fence / image-available
    /// セマフォを flight 単位に多重化する (render-finished は swapchain image
    /// 単位)。
    uint32_t    frames_in_flight = 2;
};

/// Manages the Vulkan instance, physical/logical device, queue,
/// surface and swapchain.
///
/// Ownership model:
///   - VulkanContext **owns** VkInstance, VkDevice, VkSurfaceKHR, VkSwapchainKHR.
///   - The ISurfaceProvider is **borrowed** (host retains ownership).
class VulkanContext {
public:
    VulkanContext();
    ~VulkanContext();

    VulkanContext(const VulkanContext&) = delete;
    VulkanContext& operator=(const VulkanContext&) = delete;

    /// Full initialization: instance → surface → device → swapchain.
    bool initialize(ISurfaceProvider* provider, const VulkanContextConfig& cfg = {});

    /// Tear down everything in reverse order.
    void shutdown();

    /// Recreate swapchain (e.g. after window resize).
    bool recreate_swapchain();

    /// Acquire the next swapchain image. Returns image index, or UINT32_MAX on failure.
    uint32_t acquire_next_image();

    /// Present the given swapchain image.
    bool present(uint32_t image_index);

    /// Wait for device idle.
    void device_wait_idle();

    bool is_initialized() const { return initialized_; }

#ifdef PICTOR_HAS_VULKAN
    VkInstance       instance()        const { return instance_; }
    VkPhysicalDevice physical_device() const { return physical_device_; }
    VkDevice         device()          const { return device_; }
    VkQueue          graphics_queue()  const { return graphics_queue_; }
    uint32_t         queue_family()    const { return queue_family_index_; }
    VkSwapchainKHR   swapchain()       const { return swapchain_; }
    VkFormat         swapchain_format()const { return swapchain_format_; }
    VkExtent2D       swapchain_extent()const { return swapchain_extent_; }
    const std::vector<VkImageView>& swapchain_image_views() const { return swapchain_image_views_; }
    VkRenderPass     default_render_pass() const { return render_pass_; }
    const std::vector<VkFramebuffer>& framebuffers() const { return framebuffers_; }
    VkCommandPool    command_pool() const { return command_pool_; }
    /// swapchain image ごとの primary command buffer。 長さは常に現在の
    /// swapchain image 数と一致する (recreate_swapchain() で作り直される)。
    /// consumer は acquire_next_image() が返した index で添字してよい。
    const std::vector<VkCommandBuffer>& command_buffers() const {
        return per_image_.command_buffers();
    }
    /// 現在の swapchain image 数。 recreate_swapchain() で変わりうる。
    uint32_t swapchain_image_count() const { return per_image_.image_count(); }

    // Single-time command buffer helpers (for uploads, layout transitions, etc.)
    VkCommandBuffer begin_single_time_commands();
    void            end_single_time_commands(VkCommandBuffer cmd);

    // Per-frame sync objects (frames-in-flight, Q-3)。
    //   image_available / in_flight_fence は **現在の flight** のオブジェクトを返す。
    //   render_finished は **直近に acquire した swapchain image** のセマフォを返す
    //   (consumer は acquire_next_image() の後に submit を組むので image index は確定済)。
    //   image 単位のオブジェクトは swapchain image 数が変わっても
    //   recreate_swapchain() が同じ個数へ作り直すので、 添字は常に有効。
    // これにより consumer 側のコード (acquire → submit(accessor) → present) は無変更で
    // 多重 flight 化できる。
    VkSemaphore image_available_semaphore() const { return image_available_sems_[current_frame_]; }
    VkSemaphore render_finished_semaphore() const {
        return per_image_.render_finished(last_acquired_image_);
    }
    VkFence     in_flight_fence()           const { return in_flight_fences_[current_frame_]; }

    /// 同時フレーム数 (frames-in-flight)。 consumer が flight 単位の per-frame
    /// リソース (uniform ring 等) を確保するときの段数に使える。
    uint32_t    frames_in_flight() const { return frames_in_flight_; }
    /// 現在の flight index (0..frames_in_flight()-1)。 present のたびに進む。
    uint32_t    current_frame()    const { return current_frame_; }

    // Optional extension features that the device was created with. Pictor
    // probes these during device creation and enables whichever are
    // supported. Consumers (e.g. the Rive renderer wrapper) query these to
    // pick the most efficient GPU path.
    //
    // fragment_shader_interlock (VK_EXT_fragment_shader_interlock):
    //   Lets Rive use pixel-interlock mode — single-pass coverage without
    //   atomic compute. Supported on NVIDIA Turing+, Intel Gen9+, AMD RDNA+.
    //
    // rasterization_order_attachment_access
    //   (VK_EXT_rasterization_order_attachment_access):
    //   Lets Rive use raster-ordered ROV — also single-pass, slightly
    //   cheaper than interlock on some drivers.
    bool has_fragment_shader_interlock()               const { return has_fragment_shader_interlock_; }
    bool has_rasterization_order_attachment_access()   const { return has_rasterization_order_attachment_access_; }

    /// 物理デバイスのメモリ構成を Vk 非依存記述に変換する (UMA/ReBAR 判定の入力)。
    DeviceMemoryDesc    describe_device_memory() const;
    /// 物理デバイスから直接アップロード戦略を判定する
    /// (= analyze_device_memory(describe_device_memory()))。
    DeviceMemoryProfile device_memory_profile() const;
#endif

private:
#ifdef PICTOR_HAS_VULKAN
    bool create_instance(const VulkanContextConfig& cfg);
    bool create_surface();
    bool pick_physical_device();
    bool create_logical_device();
    bool create_swapchain();
    bool create_image_views();
    bool create_render_pass();
    bool create_framebuffers();
    bool create_command_pool();
    /// swapchain image 数に追従する per-image リソースを作り直す
    /// (command buffer / render-finished セマフォ / images-in-flight 追跡)。
    /// 失敗時は既存の一式が保たれる (PerImageResourcePool が replacement-
    /// before-retire で差し替えるため)。 呼ぶ前に device idle を取ること。
    bool rebuild_per_image_resources();
    bool create_sync_objects();
    void cleanup_swapchain();
    /// swapchain + per-image リソースを畳んで、 半端な状態を公開しないようにする。
    void discard_swapchain_resources();

    ISurfaceProvider* provider_ = nullptr;

    VkInstance               instance_           = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT debug_messenger_    = VK_NULL_HANDLE;
    VkSurfaceKHR             surface_            = VK_NULL_HANDLE;
    VkPhysicalDevice         physical_device_    = VK_NULL_HANDLE;
    VkDevice                 device_             = VK_NULL_HANDLE;
    VkQueue                  graphics_queue_     = VK_NULL_HANDLE;
    VkQueue                  present_queue_      = VK_NULL_HANDLE;
    uint32_t                 queue_family_index_ = UINT32_MAX;

    VkSwapchainKHR           swapchain_          = VK_NULL_HANDLE;
    VkFormat                 swapchain_format_   = VK_FORMAT_B8G8R8A8_SRGB;
    VkExtent2D               swapchain_extent_   = {0, 0};
    std::vector<VkImage>     swapchain_images_;
    std::vector<VkImageView> swapchain_image_views_;

    VkRenderPass             render_pass_        = VK_NULL_HANDLE;
    std::vector<VkFramebuffer> framebuffers_;
    /// initialize 時の cfg を保存する (recreate_swapchain で再利用)。
    /// `create_default_render_pass=false` で起動した host は recreate でも
    /// default RP を作らない方が一貫する。
    bool                     create_default_rp_on_init_ = true;

    VkCommandPool            command_pool_       = VK_NULL_HANDLE;

    /// swapchain image 数に追従する per-image リソースの所有者。
    /// command buffer / render-finished セマフォ / images-in-flight 追跡を
    /// 1 箇所へ閉じ込め、 recreate_swapchain() から 1 回の rebuild で揃える。
    PerImageResourcePool<VulkanPerImageBackend> per_image_;

    // frames-in-flight 同期 (Q-3)。 image_available / in_flight_fence は flight
    // 単位。 render_finished と images-in-flight 追跡は swapchain image 単位なので
    // per_image_ が持つ (flight 数と image 数は一致しない前提)。
    uint32_t                 frames_in_flight_    = 2;
    uint32_t                 current_frame_       = 0;
    uint32_t                 last_acquired_image_ = 0;
    std::vector<VkSemaphore> image_available_sems_;  // [frames_in_flight_]
    std::vector<VkFence>     in_flight_fences_;      // [frames_in_flight_]

    bool has_fragment_shader_interlock_             = false;
    bool has_rasterization_order_attachment_access_ = false;
#endif

    bool initialized_ = false;
};

} // namespace pictor
