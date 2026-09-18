#pragma once

#include "vr_vulkan_util.h"
#include "pictor/core/types.h"

#include <string>
#include <vector>

namespace vr_demo {

/// 椅子に座って見渡す部屋のサンプルシーン (箱だけで組んだ汎用の部屋)。
///
/// 物体は「静的層 (部屋・家具)」 と 「動的層 (動く箱)」 に分かれる。 差分レンダリングは
/// 静的層だけを再投影で使い回し、 動的層は毎フレーム描く。 この区分けが host の責務。
class VrSceneRenderer {
public:
    enum class Layer { Static, Dynamic };

    VrSceneRenderer() = default;
    ~VrSceneRenderer();

    VrSceneRenderer(const VrSceneRenderer&)            = delete;
    VrSceneRenderer& operator=(const VrSceneRenderer&) = delete;

    /// `multiview_pass` が VK_NULL_HANDLE なら multiview 用の pipeline は作らない。
    bool initialize(VkPhysicalDevice physical, VkDevice device, VkRenderPass eye_pass,
                    VkRenderPass multiview_pass, const std::string& shader_dir);
    void shutdown();

    /// 動的層の物体を時刻に合わせて動かす。 コマンド記録の前に呼ぶ。
    void update(float time_seconds);

    /// 眼ごとに描く。 viewport / scissor は `extent` の全面。 深度テストは LESS なので、
    /// 再投影の後に呼ぶと穴の画素だけがシェーディングされる。
    void draw(VkCommandBuffer cmd, Layer layer, const pictor::float4x4& view_proj,
              VkExtent2D extent) const;

    /// multiview の render pass の中で、 両眼ぶんを 1 回で描く。
    void draw_multiview(VkCommandBuffer cmd, Layer layer,
                        const pictor::float4x4 view_proj[2], VkExtent2D extent) const;

private:
    struct Instance {
        pictor::float4x4 model;
        float            color[4];
    };

    bool create_pipeline_(VkRenderPass pass, const std::string& vert_path,
                          const std::string& frag_path, VkPipeline& out);
    void build_room_();
    void record_(VkCommandBuffer cmd, VkPipeline pipeline, Layer layer,
                 const pictor::float4x4* view_proj, uint32_t matrix_count,
                 VkExtent2D extent) const;

    VkDevice         device_             = VK_NULL_HANDLE;
    VkPipelineLayout layout_             = VK_NULL_HANDLE;
    VkPipeline       eye_pipeline_       = VK_NULL_HANDLE;
    VkPipeline       multiview_pipeline_ = VK_NULL_HANDLE;
    Buffer           cube_vertices_;
    Buffer           instance_buffer_;

    // 静的層が先頭、 動的層が末尾に並ぶ 1 本の配列 (描画は範囲指定だけで済む)。
    std::vector<Instance> instances_;
    uint32_t              static_count_ = 0;
};

} // namespace vr_demo
