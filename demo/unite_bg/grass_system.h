#pragma once
// =============================================================================
// 草システム (Scene 8)
//   ・配置データ (本家は Houdini でペイント → ここでは手続き生成で代用)
//   ・草は GameObject にしない: 配置データを GPU バッファへそのまま渡す
//   ・草の位置をセルに分割。セルは「配列の開始番号と長さ」だけを持つ
//   ・CPU: 視錐台で見えるセルを判定 → 連続するセルを 1 つにまとめて Dispatch
//   ・GPU: ComputeShader で 1 本単位のカリング + LOD 振り分け → DrawIndirect
//   ・建物: 草カリング用 Collider に入る草を除外。
//           建築中の Collider は完成後より大きいので、元データから再計算する
// =============================================================================

#ifdef PICTOR_HAS_VULKAN

#include "demo_math.h"
#include "vk_helpers.h"
#include <vector>

namespace pictor { class VulkanContext; }

namespace ubg {

struct GrassInstanceGPU {     // grass_common.glsl の GrassInstance と一致
    float pos_scale[4];
    float params[4];
};

struct GrassCell {
    uint32_t start = 0, count = 0;     // 配列の開始番号と長さ
    Vec3     bmin, bmax;               // バウンディングボックス
};

enum class BuildingState { Empty = 0, Constructing = 1, Built = 2 };

struct GrassCullStats {
    uint32_t total_instances = 0;
    uint32_t visible_cells = 0;
    uint32_t total_cells = 0;
    uint32_t dispatches = 0;
    uint32_t candidate_instances = 0;
};

class GrassSystem {
public:
    static constexpr int   kCellsPerSide = 16;
    static constexpr float kAreaHalf     = 80.0f;
    static constexpr uint32_t kLod0Verts = 5 * (4 * 2 + 1) * 3;
    static constexpr uint32_t kLod1Verts = 3 * (1 * 2 + 1) * 3;

    /// 配置データを生成 (= ペイントした結果)
    void generate();

    /// 建物の状態を変えて、Collider に入る草を除外し直す。
    /// from_original = true : 元の配置データから再計算 (正しい)
    /// from_original = false: 今のデータから更に削るだけ (Collider が縮むと穴が残る)
    void set_building(BuildingState s, bool from_original);
    BuildingState building() const { return building_; }
    Vec3 building_center() const { return {6.0f, 0.0f, 14.0f}; }
    Vec3 building_half() const;

    bool create_gpu(pictor::VulkanContext& ctx, VkDescriptorSetLayout set1_layout,
                    VkDescriptorPool pool, uint32_t flights);
    void destroy(VkDevice dev);
    /// 配置データを GPU に送る (デバイスがアイドルな時に呼ぶ)
    void upload();

    /// レンダーパスの外で: 環境光計算 + セル判定 + カリング Dispatch
    void record_compute(VkCommandBuffer cmd, uint32_t flight, const Mat4& view_proj, Vec3 cam,
                        float fade_end, float lod_dist, bool gpu_cull,
                        VkPipeline env_pipe, VkPipeline cull_pipe, VkPipelineLayout comp_layout,
                        VkDescriptorSet set0);
    /// レンダーパスの中で: LOD0 / LOD1 を DrawIndirect
    void record_draw(VkCommandBuffer cmd, uint32_t flight, VkPipeline pipe, VkPipelineLayout layout,
                     VkDescriptorSet set0);

    const GrassCullStats& stats() const { return stats_; }

private:
    void rebuild_cells();

    std::vector<GrassInstanceGPU> original_;   // ペイントしたままの配置データ
    std::vector<GrassInstanceGPU> current_;    // 建物で除外した後 (セル順に並ぶ)
    std::vector<GrassCell>        cells_;
    BuildingState building_ = BuildingState::Empty;

    VkDevice  device_ = VK_NULL_HANDLE;
    pictor::VulkanContext* ctx_ = nullptr;
    uint32_t  capacity_ = 0;
    Buffer    instances_;
    std::vector<Buffer> visible_, indirect_, env_;
    std::vector<VkDescriptorSet> sets_;
    GrassCullStats stats_;
};

} // namespace ubg

#endif
