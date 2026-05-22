#pragma once

/// VertexLayout → Vulkan 変換ヘルパー (`spec/rendering-extensibility-design.md` §6.2)。
///
/// `core/types.h` の `VertexLayout` (Vulkan 非依存のデータ) を、
/// graphics pipeline 生成に要る `VkVertexInputBindingDescription` /
/// `VkVertexInputAttributeDescription` の組へ変換する。
///
/// phase 2 のスコープ:
///   - 単一 binding (binding=0) のみ。 複数頂点バッファ / instance rate は
///     将来。
///   - `location` は attribute の登録順 (index) をそのまま割り当てる。
///     SPIR-V `layout(location=N)` との突き合わせは「def に明示記述」 のみで、
///     検証 (validation layer) に委ねる。 SPIR-V reflection は将来。
///
/// Vulkan 依存ヘッダなので `PICTOR_HAS_VULKAN` 有効時のみ実体を持つ。

#include "pictor/core/types.h"

#ifdef PICTOR_HAS_VULKAN

#include <vulkan/vulkan.h>

#include <cstdio>
#include <vector>

namespace pictor {

/// `VertexAttributeType` を対応する `VkFormat` へ写す。
inline VkFormat to_vk_format(VertexAttributeType type) {
    switch (type) {
        case VertexAttributeType::FLOAT:    return VK_FORMAT_R32_SFLOAT;
        case VertexAttributeType::FLOAT2:   return VK_FORMAT_R32G32_SFLOAT;
        case VertexAttributeType::FLOAT3:   return VK_FORMAT_R32G32B32_SFLOAT;
        case VertexAttributeType::FLOAT4:   return VK_FORMAT_R32G32B32A32_SFLOAT;
        case VertexAttributeType::UINT32:   return VK_FORMAT_R32_UINT;
        case VertexAttributeType::INT32:    return VK_FORMAT_R32_SINT;
        case VertexAttributeType::UNORM8X4: return VK_FORMAT_R8G8B8A8_UNORM;
        case VertexAttributeType::HALF2:    return VK_FORMAT_R16G16_SFLOAT;
        case VertexAttributeType::HALF4:    return VK_FORMAT_R16G16B16A16_SFLOAT;
        case VertexAttributeType::UINT32X4: return VK_FORMAT_R32G32B32A32_UINT;
        default:                            return VK_FORMAT_UNDEFINED;
    }
}

/// `VertexLayout` を `VkVertexInputBindingDescription` /
/// `VkVertexInputAttributeDescription` の組へ展開した結果。
/// `VkPipelineVertexInputStateCreateInfo` を埋めるための受け皿。
struct VkVertexInputLayout {
    VkVertexInputBindingDescription                bindings[1]{};
    uint32_t                                       binding_count = 0;
    std::vector<VkVertexInputAttributeDescription> attributes;

    /// 頂点入力空 (= phase 1 の `gl_VertexIndex` 駆動) か。
    bool empty() const { return binding_count == 0; }

    /// `VkPipelineVertexInputStateCreateInfo` を本レイアウトから埋める。
    /// `bindings` / `attributes` は本オブジェクトの生存期間に紐づくため、
    /// 返した CreateInfo は `VkVertexInputLayout` を破棄する前に使うこと。
    VkPipelineVertexInputStateCreateInfo make_create_info() const {
        VkPipelineVertexInputStateCreateInfo vi{
            VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        vi.vertexBindingDescriptionCount   = binding_count;
        vi.pVertexBindingDescriptions      = binding_count ? bindings : nullptr;
        vi.vertexAttributeDescriptionCount =
            static_cast<uint32_t>(attributes.size());
        vi.pVertexAttributeDescriptions    =
            attributes.empty() ? nullptr : attributes.data();
        return vi;
    }
};

/// `VertexLayout` を Vulkan の頂点入力記述へ変換する。
///
/// 空レイアウト (`layout.empty()`) は空の `VkVertexInputLayout` を返す
/// — phase 1 互換のフォールバック (`gl_VertexIndex` 駆動)。
///
/// `binding` は頂点バッファのバインドポイント (既定 0)。 `location` は
/// attribute の登録順をそのまま割り当てる。
inline VkVertexInputLayout to_vk_vertex_input(const VertexLayout& layout,
                                              uint32_t            binding = 0) {
    VkVertexInputLayout out;
    if (layout.empty()) return out;  // phase 1 互換: 頂点入力空

    out.binding_count          = 1;
    out.bindings[0].binding    = binding;
    out.bindings[0].stride     = layout.computed_stride();
    out.bindings[0].inputRate  = VK_VERTEX_INPUT_RATE_VERTEX;

    out.attributes.reserve(layout.attributes.size());
    for (uint32_t i = 0; i < layout.attributes.size(); ++i) {
        const VertexAttribute& a = layout.attributes[i];
        const VkFormat fmt = to_vk_format(a.type);
        if (fmt == VK_FORMAT_UNDEFINED) {
            std::fprintf(stderr,
                         "[vertex_layout] unsupported attribute type for "
                         "location %u — skipped\n", i);
            continue;
        }
        VkVertexInputAttributeDescription d{};
        d.location = i;
        d.binding  = binding;
        d.format   = fmt;
        d.offset   = a.offset;
        out.attributes.push_back(d);
    }
    return out;
}

} // namespace pictor

#endif // PICTOR_HAS_VULKAN
