#include "pictor/pipeline/attachment_def.h"

#include <algorithm>

namespace pictor {

// ============================================================
// to_string
// ============================================================

const char* to_string(AttachmentKind k) {
    switch (k) {
        case AttachmentKind::COLOR:           return "COLOR";
        case AttachmentKind::DEPTH:           return "DEPTH";
        case AttachmentKind::SWAPCHAIN_COLOR: return "SWAPCHAIN_COLOR";
    }
    return "COLOR";
}

const char* to_string(AttachmentSizing s) {
    switch (s) {
        case AttachmentSizing::SWAPCHAIN_RELATIVE: return "SWAPCHAIN_RELATIVE";
        case AttachmentSizing::ABSOLUTE:           return "ABSOLUTE";
    }
    return "SWAPCHAIN_RELATIVE";
}

const char* to_string(AttachmentFormat f) {
    switch (f) {
        case AttachmentFormat::UNDEFINED:           return "UNDEFINED";
        case AttachmentFormat::R8G8B8A8_UNORM:      return "R8G8B8A8_UNORM";
        case AttachmentFormat::R8G8B8A8_SRGB:       return "R8G8B8A8_SRGB";
        case AttachmentFormat::B8G8R8A8_UNORM:      return "B8G8R8A8_UNORM";
        case AttachmentFormat::B8G8R8A8_SRGB:       return "B8G8R8A8_SRGB";
        case AttachmentFormat::R16G16B16A16_SFLOAT: return "R16G16B16A16_SFLOAT";
        case AttachmentFormat::R32G32B32A32_SFLOAT: return "R32G32B32A32_SFLOAT";
        case AttachmentFormat::R11G11B10_UFLOAT:    return "R11G11B10_UFLOAT";
        case AttachmentFormat::D16_UNORM:           return "D16_UNORM";
        case AttachmentFormat::D24_UNORM_S8_UINT:   return "D24_UNORM_S8_UINT";
        case AttachmentFormat::D32_SFLOAT:          return "D32_SFLOAT";
        case AttachmentFormat::D32_SFLOAT_S8_UINT:  return "D32_SFLOAT_S8_UINT";
    }
    return "UNDEFINED";
}

const char* to_string(AttachmentLoadOp op) {
    switch (op) {
        case AttachmentLoadOp::LOAD:      return "LOAD";
        case AttachmentLoadOp::CLEAR:     return "CLEAR";
        case AttachmentLoadOp::DONT_CARE: return "DONT_CARE";
        case AttachmentLoadOp::NONE:      return "NONE";
    }
    return "CLEAR";
}

const char* to_string(AttachmentStoreOp op) {
    switch (op) {
        case AttachmentStoreOp::STORE:     return "STORE";
        case AttachmentStoreOp::DONT_CARE: return "DONT_CARE";
        case AttachmentStoreOp::NONE:      return "NONE";
    }
    return "STORE";
}

const char* to_string(ImageLayout l) {
    switch (l) {
        case ImageLayout::UNDEFINED:                        return "UNDEFINED";
        case ImageLayout::GENERAL:                          return "GENERAL";
        case ImageLayout::COLOR_ATTACHMENT_OPTIMAL:         return "COLOR_ATTACHMENT_OPTIMAL";
        case ImageLayout::DEPTH_STENCIL_ATTACHMENT_OPTIMAL: return "DEPTH_STENCIL_ATTACHMENT_OPTIMAL";
        case ImageLayout::DEPTH_STENCIL_READ_ONLY_OPTIMAL:  return "DEPTH_STENCIL_READ_ONLY_OPTIMAL";
        case ImageLayout::SHADER_READ_ONLY_OPTIMAL:         return "SHADER_READ_ONLY_OPTIMAL";
        case ImageLayout::TRANSFER_SRC_OPTIMAL:             return "TRANSFER_SRC_OPTIMAL";
        case ImageLayout::TRANSFER_DST_OPTIMAL:             return "TRANSFER_DST_OPTIMAL";
        case ImageLayout::PRESENT_SRC_KHR:                  return "PRESENT_SRC_KHR";
    }
    return "UNDEFINED";
}

// ============================================================
// parse — case sensitive (canonical spelling only)
// ============================================================

namespace {
bool eq(std::string_view s, const char* lit) {
    return s == std::string_view(lit);
}
} // namespace

bool parse_attachment_kind(std::string_view s, AttachmentKind& out) {
    if (eq(s, "COLOR"))           { out = AttachmentKind::COLOR;           return true; }
    if (eq(s, "DEPTH"))           { out = AttachmentKind::DEPTH;           return true; }
    if (eq(s, "SWAPCHAIN_COLOR")) { out = AttachmentKind::SWAPCHAIN_COLOR; return true; }
    return false;
}

bool parse_attachment_sizing(std::string_view s, AttachmentSizing& out) {
    if (eq(s, "SWAPCHAIN_RELATIVE")) { out = AttachmentSizing::SWAPCHAIN_RELATIVE; return true; }
    if (eq(s, "ABSOLUTE"))           { out = AttachmentSizing::ABSOLUTE;           return true; }
    return false;
}

bool parse_attachment_format(std::string_view s, AttachmentFormat& out) {
    if (eq(s, "UNDEFINED"))           { out = AttachmentFormat::UNDEFINED;           return true; }
    if (eq(s, "R8G8B8A8_UNORM"))      { out = AttachmentFormat::R8G8B8A8_UNORM;      return true; }
    if (eq(s, "R8G8B8A8_SRGB"))       { out = AttachmentFormat::R8G8B8A8_SRGB;       return true; }
    if (eq(s, "B8G8R8A8_UNORM"))      { out = AttachmentFormat::B8G8R8A8_UNORM;      return true; }
    if (eq(s, "B8G8R8A8_SRGB"))       { out = AttachmentFormat::B8G8R8A8_SRGB;       return true; }
    if (eq(s, "R16G16B16A16_SFLOAT")) { out = AttachmentFormat::R16G16B16A16_SFLOAT; return true; }
    if (eq(s, "R32G32B32A32_SFLOAT")) { out = AttachmentFormat::R32G32B32A32_SFLOAT; return true; }
    if (eq(s, "R11G11B10_UFLOAT"))    { out = AttachmentFormat::R11G11B10_UFLOAT;    return true; }
    if (eq(s, "D16_UNORM"))           { out = AttachmentFormat::D16_UNORM;           return true; }
    if (eq(s, "D24_UNORM_S8_UINT"))   { out = AttachmentFormat::D24_UNORM_S8_UINT;   return true; }
    if (eq(s, "D32_SFLOAT"))          { out = AttachmentFormat::D32_SFLOAT;          return true; }
    if (eq(s, "D32_SFLOAT_S8_UINT"))  { out = AttachmentFormat::D32_SFLOAT_S8_UINT;  return true; }
    return false;
}

bool parse_attachment_load_op(std::string_view s, AttachmentLoadOp& out) {
    if (eq(s, "LOAD"))      { out = AttachmentLoadOp::LOAD;      return true; }
    if (eq(s, "CLEAR"))     { out = AttachmentLoadOp::CLEAR;     return true; }
    if (eq(s, "DONT_CARE")) { out = AttachmentLoadOp::DONT_CARE; return true; }
    if (eq(s, "NONE"))      { out = AttachmentLoadOp::NONE;      return true; }
    return false;
}

bool parse_attachment_store_op(std::string_view s, AttachmentStoreOp& out) {
    if (eq(s, "STORE"))     { out = AttachmentStoreOp::STORE;     return true; }
    if (eq(s, "DONT_CARE")) { out = AttachmentStoreOp::DONT_CARE; return true; }
    if (eq(s, "NONE"))      { out = AttachmentStoreOp::NONE;      return true; }
    return false;
}

bool parse_image_layout(std::string_view s, ImageLayout& out) {
    if (eq(s, "UNDEFINED"))                        { out = ImageLayout::UNDEFINED;                        return true; }
    if (eq(s, "GENERAL"))                          { out = ImageLayout::GENERAL;                          return true; }
    if (eq(s, "COLOR_ATTACHMENT_OPTIMAL"))         { out = ImageLayout::COLOR_ATTACHMENT_OPTIMAL;         return true; }
    if (eq(s, "DEPTH_STENCIL_ATTACHMENT_OPTIMAL")) { out = ImageLayout::DEPTH_STENCIL_ATTACHMENT_OPTIMAL; return true; }
    if (eq(s, "DEPTH_STENCIL_READ_ONLY_OPTIMAL"))  { out = ImageLayout::DEPTH_STENCIL_READ_ONLY_OPTIMAL;  return true; }
    if (eq(s, "SHADER_READ_ONLY_OPTIMAL"))         { out = ImageLayout::SHADER_READ_ONLY_OPTIMAL;         return true; }
    if (eq(s, "TRANSFER_SRC_OPTIMAL"))             { out = ImageLayout::TRANSFER_SRC_OPTIMAL;             return true; }
    if (eq(s, "TRANSFER_DST_OPTIMAL"))             { out = ImageLayout::TRANSFER_DST_OPTIMAL;             return true; }
    if (eq(s, "PRESENT_SRC_KHR"))                  { out = ImageLayout::PRESENT_SRC_KHR;                  return true; }
    return false;
}

// ============================================================
// Predicates
// ============================================================

bool is_depth_format(AttachmentFormat f) {
    switch (f) {
        case AttachmentFormat::D16_UNORM:
        case AttachmentFormat::D24_UNORM_S8_UINT:
        case AttachmentFormat::D32_SFLOAT:
        case AttachmentFormat::D32_SFLOAT_S8_UINT:
            return true;
        default:
            return false;
    }
}

bool is_valid_format(AttachmentFormat f) {
    return f != AttachmentFormat::UNDEFINED;
}

// ============================================================
// Default tables — kept in sync with Ergo `default_attachments.ts`
// (any change here MUST be mirrored there).
// ============================================================

std::array<AttachmentDef, 3> default_attachments() {
    AttachmentDef hdr_color;
    hdr_color.name   = "scene_hdr_color";
    hdr_color.kind   = AttachmentKind::COLOR;
    hdr_color.format = AttachmentFormat::R16G16B16A16_SFLOAT;
    hdr_color.sizing = AttachmentSizing::SWAPCHAIN_RELATIVE;
    hdr_color.scale  = 1.0f;
    hdr_color.usage  = USAGE_COLOR_ATTACHMENT | USAGE_SAMPLED;
    hdr_color.clear_color = {{0.0f, 0.0f, 0.0f, 1.0f}};

    AttachmentDef depth;
    depth.name   = "scene_depth";
    depth.kind   = AttachmentKind::DEPTH;
    depth.format = AttachmentFormat::D32_SFLOAT;
    depth.sizing = AttachmentSizing::SWAPCHAIN_RELATIVE;
    depth.scale  = 1.0f;
    depth.usage  = USAGE_DEPTH_STENCIL_ATTACHMENT;
    depth.clear_depth   = 1.0f;
    depth.clear_stencil = 0;

    AttachmentDef swap;
    swap.name   = "swapchain";
    swap.kind   = AttachmentKind::SWAPCHAIN_COLOR;
    swap.format = AttachmentFormat::B8G8R8A8_SRGB; // overwritten at runtime
    swap.sizing = AttachmentSizing::SWAPCHAIN_RELATIVE;
    swap.scale  = 1.0f;
    swap.usage  = USAGE_COLOR_ATTACHMENT;
    swap.clear_color = {{0.0f, 0.0f, 0.0f, 1.0f}};

    return { hdr_color, depth, swap };
}

std::vector<AttachmentOpsDef> default_attachment_ops(
        const std::vector<std::string>& targets,
        const std::vector<AttachmentDef>& attachments)
{
    std::vector<AttachmentOpsDef> ops;
    ops.reserve(targets.size());

    for (const auto& tgt : targets) {
        AttachmentOpsDef op;
        op.attachment_name = tgt;

        // Look up the def to choose layout defaults.
        const AttachmentDef* def = nullptr;
        for (const auto& a : attachments) {
            if (a.name == tgt) { def = &a; break; }
        }

        if (def && def->kind == AttachmentKind::DEPTH) {
            op.load_op        = AttachmentLoadOp::CLEAR;
            op.store_op       = AttachmentStoreOp::DONT_CARE;
            op.initial_layout = ImageLayout::UNDEFINED;
            op.final_layout   = ImageLayout::DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        } else if (def && def->kind == AttachmentKind::SWAPCHAIN_COLOR) {
            op.load_op        = AttachmentLoadOp::CLEAR;
            op.store_op       = AttachmentStoreOp::STORE;
            op.initial_layout = ImageLayout::UNDEFINED;
            op.final_layout   = ImageLayout::PRESENT_SRC_KHR;
        } else {
            // COLOR (or unknown — treat as color)
            op.load_op        = AttachmentLoadOp::CLEAR;
            op.store_op       = AttachmentStoreOp::STORE;
            op.initial_layout = ImageLayout::UNDEFINED;
            op.final_layout   = ImageLayout::SHADER_READ_ONLY_OPTIMAL;
        }

        ops.push_back(std::move(op));
    }

    return ops;
}

} // namespace pictor
