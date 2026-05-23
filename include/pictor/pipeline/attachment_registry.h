#pragma once

/// AttachmentRegistry — named GPU attachments resolved by `uint16_t` index.
///
/// `spec/pipeline-system-b-config.md` §3.3。 hot path から `std::string` を
/// 完全排除するため、 init 時に `index_of()` で名前を `uint16_t` へ畳み込み、
/// 以降は index で `view()` / `image()` / `format()` を引く。
///
/// Vulkan 依存部分は `PICTOR_HAS_VULKAN` ガード。 ガードなしでも
/// AttachmentDef 配列を保持して名前↔index の解決はできる (テスト / 編集
/// ツール / host process 連携用)。

#include "pictor/pipeline/attachment_def.h"

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#ifdef PICTOR_HAS_VULKAN
// On Windows, <vulkan/vulkan.h> transitively pulls <windows.h>, which
// otherwise `#define`s GDI macros like `OPAQUE` / `TRANSPARENT` that collide
// with our enum values (`PassType::OPAQUE`). Suppress the GDI portion.
#ifndef NOGDI
#define NOGDI
#endif
#include <vulkan/vulkan.h>
#endif

namespace pictor {

class AttachmentRegistry {
public:
    static constexpr uint16_t INVALID_INDEX = 0xFFFFu;
    static constexpr uint32_t MAX_FLIGHTS   = 4u;
    static constexpr uint32_t MAX_SWAPCHAIN_IMAGES = 8u;

    AttachmentRegistry();
    ~AttachmentRegistry();

    AttachmentRegistry(const AttachmentRegistry&)            = delete;
    AttachmentRegistry& operator=(const AttachmentRegistry&) = delete;

    // ---- Name → index (Vulkan 非依存、 init/edit 経路) -----------------

    /// Replace the attachment defs (does not touch GPU resources yet).
    /// `defs` の `name` は unique であること。 重複は最後勝ち + assert。
    void set_defs(std::span<const AttachmentDef> defs);

    /// Look up an attachment by name. Returns `INVALID_INDEX` if not found.
    /// `string_view` 受けで `std::string` を作らないため、 hot path 直前
    /// (compile 時) でも安全に呼べる。
    uint16_t index_of(std::string_view name) const;

    /// Number of registered attachments.
    uint16_t count() const { return static_cast<uint16_t>(defs_.size()); }

    /// Inspect a def by index.
    const AttachmentDef& def(uint16_t index) const { return defs_[index]; }

    /// All defs (read-only). Useful for UI / editor / serializer.
    std::span<const AttachmentDef> defs() const { return defs_; }

    // ---- Vulkan resources (PICTOR_HAS_VULKAN 時のみ) -------------------

#ifdef PICTOR_HAS_VULKAN
    /// Allocate VkImage / VkImageView for non-swapchain defs.
    /// `flight_count` は SWAPCHAIN_COLOR 以外の attachment の多重化数
    /// (per-flight)。 SWAPCHAIN_COLOR は `set_swapchain_images()` で別途
    /// inject する。 戻り値 false は GPU リソース確保失敗 (validation /
    /// out-of-memory)。
    bool initialize_vulkan(VkPhysicalDevice physical_device,
                           VkDevice         device,
                           VkExtent2D       swapchain_extent,
                           uint32_t         flight_count);

    /// Resize all non-swapchain attachments. swapchain は host が
    /// `set_swapchain_images()` で再 inject する。
    bool resize(VkExtent2D new_extent);

    /// Inject swapchain images (called by VulkanContext on
    /// `recreate_swapchain`)。 `views.size() == images.size()` を期待。
    void set_swapchain_images(std::span<const VkImage>     images,
                              std::span<const VkImageView> views,
                              VkFormat                     format,
                              VkExtent2D                   extent);

    /// Replace an attachment with caller-owned VkImage/VkImageView handles.
    ///
    /// Use when the host already owns the GPU resources (e.g. KS の
    /// `PostProcessLayer` が `PostProcessPipeline` 内部で先に作った
    /// `scene_hdr_color`) と Phase 4 で AttachmentRegistry に参照させたい
    /// 場合。 `name` で attachment を引き、 内部で作成済みだった
    /// VkImage/View/Memory を破棄して caller-owned handles を割り当てる。
    ///
    /// `images.size() == views.size() == flight_count_` を期待 (per-flight)。
    /// 以後 `image()` / `view()` は提供された handle を返し、 `shutdown_vulkan()`
    /// では destroy しない (caller の所有権)。 `resize()` も触らない
    /// (host が swapchain recreate 後に同 API で再 inject する)。
    ///
    /// 戻り値 false: 該当 name が未登録、 もしくは登録済みだが SWAPCHAIN_COLOR
    /// (こちらは set_swapchain_images を使うこと)。
    bool set_external_attachment(std::string_view             name,
                                 std::span<const VkImage>     images,
                                 std::span<const VkImageView> views,
                                 VkFormat                     format,
                                 VkExtent2D                   extent);

    /// Free all Vulkan resources owned by the registry.
    void shutdown_vulkan();

    /// Query helpers (hot-path safe — single array index lookup).
    /// SWAPCHAIN_COLOR の場合 `slot_index` は swapchain image index、
    /// それ以外は flight_index。
    VkImageView view (uint16_t att_index, uint32_t slot_index) const;
    VkImage     image(uint16_t att_index, uint32_t slot_index) const;
    VkFormat    vk_format(uint16_t att_index) const;
    VkExtent2D  extent(uint16_t att_index) const;
    uint32_t    slot_count(uint16_t att_index) const; // flight or swapchain image count

    VkDevice device() const { return device_; }
    VkExtent2D current_extent() const { return current_extent_; }
    uint32_t flight_count() const { return flight_count_; }
    uint32_t swapchain_image_count() const { return swapchain_image_count_; }
#endif // PICTOR_HAS_VULKAN

private:
    std::vector<AttachmentDef> defs_;

#ifdef PICTOR_HAS_VULKAN
    struct Resource {
        // Non-swapchain non-external: per-flight image+view+memory (owned)
        // Swapchain                 : per-image image+view (memory not owned)
        // External                  : per-flight image+view (memory not owned)
        std::vector<VkImage>        images;        // flight_count or swapchain_image_count
        std::vector<VkImageView>    views;
        std::vector<VkDeviceMemory> memories;      // empty for swapchain/external (not owned)
        VkFormat                    vk_format = VK_FORMAT_UNDEFINED;
        VkExtent2D                  extent    = {0, 0};
        bool                        is_swapchain = false;
        bool                        is_external  = false;  // caller-owned, do not destroy
    };

    bool create_one_(uint16_t index, VkExtent2D extent);
    void destroy_one_(Resource& r);

    VkPhysicalDevice          physical_device_       = VK_NULL_HANDLE;
    VkDevice                  device_                = VK_NULL_HANDLE;
    VkExtent2D                current_extent_        = {0, 0};
    uint32_t                  flight_count_          = 0;
    uint32_t                  swapchain_image_count_ = 0;
    std::vector<Resource>     resources_;
#endif
};

#ifdef PICTOR_HAS_VULKAN
/// AttachmentFormat → VkFormat. UNDEFINED maps to VK_FORMAT_UNDEFINED.
VkFormat to_vk_format(AttachmentFormat f);

/// Common bit-set translation for VkImageUsageFlags.
VkImageUsageFlags to_vk_usage(uint32_t mask);

VkAttachmentLoadOp  to_vk_load_op(AttachmentLoadOp op);
VkAttachmentStoreOp to_vk_store_op(AttachmentStoreOp op);
VkImageLayout       to_vk_layout(ImageLayout l);
#endif

} // namespace pictor
