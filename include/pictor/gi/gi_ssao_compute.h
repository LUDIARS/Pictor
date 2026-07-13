#pragma once

/// GISsaoCompute — SSAO の実 compute 実行 (phase 2 残タスク)。
///
/// `ssao_gen.comp` (深度のみ — 法線は深度差分から再構築) を dispatch し、
/// R8 の AO マップを生成する。 マテリアルシェーダは `ao_view()` を
/// サンプルして間接光項へ乗算する (PP 近似 `ssao_apply.frag` の上位互換 —
/// 両方 enabled にしない: `spec/feature/gi-bake-realtime-design.md` §3)。
///
/// host-driven 契約:
///   1. init 後、 `set_depth_input()` にシーン深度ビュー
///      (`PostProcessPipeline::scene_depth_view()`) を渡す。 PP の
///      resize / rebuild_chain 後はビューが変わるため再度呼ぶ (GPU idle 中)。
///   2. 毎フレーム `update_camera()` (投影行列と逆行列 — 逆行列はホスト算出)
///      を書き、 シーン depth prepass 完了後に `record()` を呼ぶ。
///   3. `ao_view()` + `ao_sampler()` をマテリアル descriptor へ結線する。
///
/// 行列規約: pictor::float4x4 (row-vector) を生 memcpy — シェーダ側は
/// column-major mat4 として M*v で読むため、 転置なしで v*M と一致する。

#include "pictor/core/types.h"
#include "pictor/gi/gi_lighting_system.h"

#include <cmath>
#include <string>

#ifdef PICTOR_HAS_VULKAN
#include <vulkan/vulkan.h>
#endif

namespace pictor {

class VulkanContext;

/// SSAO 半球カーネル生成 (+Z 半球、 fibonacci 配置 — 決定的)。
/// `out` に vec4 (xyz = 方向, w = 半径スケール) × count を書く
/// (float 数 = count * 4)。 中心寄りサンプルを密にする二乗スケール。
inline void generate_ssao_kernel(uint32_t count, float* out) {
    constexpr float kGoldenAngle = 2.39996322972865332f;
    for (uint32_t i = 0; i < count; ++i) {
        // +Z 半球に概均一配置。
        const float z = (static_cast<float>(i) + 0.5f)
                      / static_cast<float>(count);
        const float r = std::sqrt(std::max(0.0f, 1.0f - z * z));
        const float phi = kGoldenAngle * static_cast<float>(i);
        // 半径スケール: 近距離を密に (古典 SSAO カーネルの慣例)。
        const float t = static_cast<float>(i) / static_cast<float>(count);
        const float scale = 0.1f + 0.9f * t * t;
        out[i * 4 + 0] = r * std::cos(phi);
        out[i * 4 + 1] = r * std::sin(phi);
        out[i * 4 + 2] = z;
        out[i * 4 + 3] = scale;
    }
}

class GISsaoCompute {
public:
    static constexpr uint32_t kMaxKernelSamples = 64;

    GISsaoCompute() = default;
    ~GISsaoCompute();

    GISsaoCompute(const GISsaoCompute&) = delete;
    GISsaoCompute& operator=(const GISsaoCompute&) = delete;

    bool is_initialized() const { return initialized_; }

#ifdef PICTOR_HAS_VULKAN
    /// AO image (R8) / kernel / UBO / compute pipeline を確保する。
    /// `shader_dir` に ssao_gen.comp.spv が必要 (無ければ false — fail-fast)。
    bool initialize(VulkanContext& vk, const std::string& shader_dir,
                    uint32_t width, uint32_t height, const SSAOConfig& config);

    /// シーン深度ビューを descriptor へ結線する。 `layout` はサンプル時の
    /// image layout (PP の scene depth は DEPTH_STENCIL_READ_ONLY_OPTIMAL)。
    /// 変更は GPU idle 中に行うこと (in-flight descriptor 更新は不可)。
    void set_depth_input(VkImageView depth_view,
                         VkImageLayout layout =
                             VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);

    /// 投影行列 / 逆行列を UBO へ書く (毎フレーム)。
    void update_camera(const float4x4& projection,
                       const float4x4& inv_projection);

    /// radius / bias / intensity 等を反映する。 sample_count 変更時は
    /// カーネルも再生成する (上限 kMaxKernelSamples)。
    void set_config(const SSAOConfig& config);

    /// AO 生成 dispatch を記録する (depth prepass 完了後に呼ぶ)。
    /// 末尾 barrier が compute write → fragment read を保証する。
    void record(VkCommandBuffer cmd);

    /// AO image を作り直す (swapchain resize 時、 GPU idle 中)。
    bool resize(uint32_t width, uint32_t height);

    // ---- マテリアル結線用 ----
    VkImageView ao_view() const    { return ao_view_; }
    VkSampler   ao_sampler() const { return ao_sampler_; }
#endif

    void shutdown();

private:
#ifdef PICTOR_HAS_VULKAN
    bool create_ao_image_(uint32_t width, uint32_t height);
    void destroy_ao_image_();
    void write_params_();

    VulkanContext* vk_     = nullptr;
    VkDevice       device_ = VK_NULL_HANDLE;

    SSAOConfig config_{};
    uint32_t   width_  = 0;
    uint32_t   height_ = 0;
    bool       ao_image_fresh_ = true;   // 初回 record の oldLayout 判定
    bool       depth_bound_    = false;  // set_depth_input 済みか (未了は no-op)

    VkImage        ao_image_  = VK_NULL_HANDLE;
    VkDeviceMemory ao_memory_ = VK_NULL_HANDLE;
    VkImageView    ao_view_   = VK_NULL_HANDLE;
    VkSampler      ao_sampler_    = VK_NULL_HANDLE;
    VkSampler      depth_sampler_ = VK_NULL_HANDLE;

    // host-visible 永続マップ (params UBO / kernel SSBO)。
    VkBuffer       params_buf_ = VK_NULL_HANDLE;
    VkDeviceMemory params_mem_ = VK_NULL_HANDLE;
    void*          params_mapped_ = nullptr;
    VkBuffer       kernel_buf_ = VK_NULL_HANDLE;
    VkDeviceMemory kernel_mem_ = VK_NULL_HANDLE;
    void*          kernel_mapped_ = nullptr;

    VkDescriptorSetLayout dsl_       = VK_NULL_HANDLE;
    VkPipelineLayout      layout_    = VK_NULL_HANDLE;
    VkPipeline            pipeline_  = VK_NULL_HANDLE;
    VkDescriptorPool      desc_pool_ = VK_NULL_HANDLE;
    VkDescriptorSet       desc_set_  = VK_NULL_HANDLE;
#endif

    bool initialized_ = false;
};

} // namespace pictor
