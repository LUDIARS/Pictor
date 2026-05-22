#pragma once

/// Attachment definitions for the pipeline profile (`spec/pipeline-system-b-config.md`).
///
/// `AttachmentDef` declares a named GPU image used by render passes
/// (`scene_hdr_color` / `scene_depth` / `swapchain` 等)。 All enums are
/// Vulkan-free (own subset) so this header can be consumed by serializers
/// and host code without depending on `<vulkan/vulkan.h>` — the
/// translation to `VkFormat` / `VkImageLayout` / etc. is done in
/// `attachment_registry.h` under the `PICTOR_HAS_VULKAN` guard.
///
/// Phase 3 (`spec/pipeline-system-b-config.md`): the editor / runtime
/// drives the real `VkRenderPass` chain from these defs. v1 profiles
/// (no `attachments[]`) get a built-in default of three attachments
/// (`scene_hdr_color` / `scene_depth` / `swapchain`).

#include "pictor/core/types.h"

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace pictor {

// ============================================================
// Attachment classification
// ============================================================

enum class AttachmentKind : uint8_t {
    COLOR           = 0,   ///< Normal color attachment (HDR / LDR target)
    DEPTH           = 1,   ///< Depth (or depth-stencil) attachment
    SWAPCHAIN_COLOR = 2,   ///< Special: runtime injects the swapchain image
};

enum class AttachmentSizing : uint8_t {
    SWAPCHAIN_RELATIVE = 0, ///< extent = swapchain_extent * scale
    ABSOLUTE           = 1, ///< extent = (width, height) — explicit
};

/// Subset of Vulkan formats we use for attachments. The registry maps
/// these to `VkFormat` at GPU-creation time. Values are stable across
/// schema revisions (do NOT reorder).
enum class AttachmentFormat : uint8_t {
    UNDEFINED            = 0,

    // ---- Color (LDR) ----
    R8G8B8A8_UNORM       = 1,
    R8G8B8A8_SRGB        = 2,
    B8G8R8A8_UNORM       = 3,
    B8G8R8A8_SRGB        = 4,

    // ---- Color (HDR / float) ----
    R16G16B16A16_SFLOAT  = 16,
    R32G32B32A32_SFLOAT  = 17,
    R11G11B10_UFLOAT     = 18,

    // ---- Depth / depth-stencil ----
    D16_UNORM            = 32,
    D24_UNORM_S8_UINT    = 33,
    D32_SFLOAT           = 34,
    D32_SFLOAT_S8_UINT   = 35,
};

/// Bitmask of image usage flags (matches `VkImageUsageFlagBits` semantics).
/// Stored as `uint32_t` in `AttachmentDef::usage`.
enum AttachmentUsageBits : uint32_t {
    USAGE_TRANSFER_SRC              = 1u << 0,
    USAGE_TRANSFER_DST              = 1u << 1,
    USAGE_SAMPLED                   = 1u << 2,
    USAGE_STORAGE                   = 1u << 3,
    USAGE_COLOR_ATTACHMENT          = 1u << 4,
    USAGE_DEPTH_STENCIL_ATTACHMENT  = 1u << 5,
    USAGE_TRANSIENT_ATTACHMENT      = 1u << 6,
    USAGE_INPUT_ATTACHMENT          = 1u << 7,
};

/// VkAttachmentLoadOp 相当。
enum class AttachmentLoadOp : uint8_t {
    LOAD      = 0,
    CLEAR     = 1,
    DONT_CARE = 2,
    NONE      = 3,
};

/// VkAttachmentStoreOp 相当。
enum class AttachmentStoreOp : uint8_t {
    STORE     = 0,
    DONT_CARE = 1,
    NONE      = 2,
};

/// VkImageLayout 相当のサブセット。
enum class ImageLayout : uint8_t {
    UNDEFINED                        = 0,
    GENERAL                          = 1,
    COLOR_ATTACHMENT_OPTIMAL         = 2,
    DEPTH_STENCIL_ATTACHMENT_OPTIMAL = 3,
    DEPTH_STENCIL_READ_ONLY_OPTIMAL  = 4,
    SHADER_READ_ONLY_OPTIMAL         = 5,
    TRANSFER_SRC_OPTIMAL             = 6,
    TRANSFER_DST_OPTIMAL             = 7,
    PRESENT_SRC_KHR                  = 8,
};

// ============================================================
// Data
// ============================================================

/// One named GPU image used as an attachment / sampled target.
/// `name` must be unique within a profile. The reserved name
/// `"swapchain"` always has `kind == SWAPCHAIN_COLOR` and the runtime
/// injects the per-image view.
struct AttachmentDef {
    std::string                  name;
    AttachmentKind               kind    = AttachmentKind::COLOR;
    AttachmentFormat             format  = AttachmentFormat::R16G16B16A16_SFLOAT;
    AttachmentSizing             sizing  = AttachmentSizing::SWAPCHAIN_RELATIVE;
    float                        scale   = 1.0f;
    uint32_t                     width   = 0;     // sizing == ABSOLUTE 用
    uint32_t                     height  = 0;     // 同上
    uint32_t                     usage   = USAGE_COLOR_ATTACHMENT | USAGE_SAMPLED;
    std::array<float, 4>         clear_color   = {{0.0f, 0.0f, 0.0f, 1.0f}};
    float                        clear_depth   = 1.0f;
    uint32_t                     clear_stencil = 0;
};

/// Per-pass override of how an attachment is used (`load/store/layout`).
/// `attachment_name` must match an entry in `PipelineProfileDef::attachments[]`
/// or the reserved name `"swapchain"`.
struct AttachmentOpsDef {
    std::string       attachment_name;
    AttachmentLoadOp  load_op        = AttachmentLoadOp::CLEAR;
    AttachmentStoreOp store_op       = AttachmentStoreOp::STORE;
    ImageLayout       initial_layout = ImageLayout::UNDEFINED;
    ImageLayout       final_layout   = ImageLayout::SHADER_READ_ONLY_OPTIMAL;
};

// ============================================================
// Helpers — name <-> enum (used by serializer & UI)
// ============================================================

const char* to_string(AttachmentKind);
const char* to_string(AttachmentSizing);
const char* to_string(AttachmentFormat);
const char* to_string(AttachmentLoadOp);
const char* to_string(AttachmentStoreOp);
const char* to_string(ImageLayout);

bool parse_attachment_kind   (std::string_view s, AttachmentKind&    out);
bool parse_attachment_sizing (std::string_view s, AttachmentSizing&  out);
bool parse_attachment_format (std::string_view s, AttachmentFormat&  out);
bool parse_attachment_load_op (std::string_view s, AttachmentLoadOp&  out);
bool parse_attachment_store_op(std::string_view s, AttachmentStoreOp& out);
bool parse_image_layout      (std::string_view s, ImageLayout&       out);

/// Test whether a format is depth (or depth-stencil).
bool is_depth_format(AttachmentFormat f);

/// `clear_depth` / `clear_color` validity — returns `false` for UNDEFINED.
bool is_valid_format(AttachmentFormat f);

/// Built-in default attachments (Phase 3 §6 fallback for v1 profiles or
/// when a v2 profile has an empty `attachments[]`). Order is significant:
/// the swapchain entry is always last so it can be patched by the runtime.
std::array<AttachmentDef, 3> default_attachments();

/// Built-in default `attachment_ops[]` for a render pass that lists `targets`.
/// Mirrors §5 / §3.2 of the Phase 3 spec — COLOR=CLEAR/STORE, DEPTH=CLEAR/
/// DONT_CARE, SWAPCHAIN=CLEAR/STORE→PRESENT_SRC_KHR.
std::vector<AttachmentOpsDef> default_attachment_ops(
        const std::vector<std::string>& targets,
        const std::vector<AttachmentDef>& attachments);

} // namespace pictor
