#pragma once

/// GIGpuExecutor — GI の実 GPU 実行 (phase 2)。
///
/// `GILightingSystem` は CPU 経路 (probe SH 保持 + CPU 補間) を持つが、
/// `GPUBufferManager` の `GpuAllocation` はアカウンティングのみで実 VkBuffer
/// を持たない (`spec/feature/gi-bake-realtime-design.md` §5)。 本クラスが
/// GI 用の実 Vulkan リソースを自己所有し、 `gi_probe_sample.comp` の
/// per-object irradiance dispatch を記録する。
///
/// host-driven 契約 (PostProcessPipeline と同型):
///   1. init 後、 ホストが毎フレーム `update_objects()` (dynamic pool の
///      transforms) と、 ライト変化時に `upload_probe_sh()`
///      (`GIProbeField::sh_data()`) を呼ぶ。
///   2. ホストのコマンドバッファへ `record()` で dispatch を記録する
///      (シーン描画より前 — barrier で compute write → shader read を保証)。
///   3. マテリアル側 (pbr_gi.frag / gi.glsl) は `params_buffer()` +
///      `probe_sh_buffer()` を descriptor へ結線して per-pixel サンプルする。
///      per-object 消費側は `object_irradiance_buffer()` を使う。
///
/// SRP: Vulkan 資源管理と dispatch のみ。 SH の意味論 (構築 / relight) は
/// `GIProbeField`、 設定は `GIProbeConfig` の責務。

#include "pictor/core/types.h"
#include "pictor/gi/gi_lighting_system.h"

#include <string>

#ifdef PICTOR_HAS_VULKAN
#include <vulkan/vulkan.h>
#endif

namespace pictor {

class VulkanContext;

class GIGpuExecutor {
public:
    GIGpuExecutor() = default;
    ~GIGpuExecutor();

    GIGpuExecutor(const GIGpuExecutor&) = delete;
    GIGpuExecutor& operator=(const GIGpuExecutor&) = delete;

    bool is_initialized() const { return initialized_; }

#ifdef PICTOR_HAS_VULKAN
    /// 実 Vulkan リソースを確保する。 `shader_dir` はコンパイル済み .spv の
    /// ディレクトリ (gi_probe_sample.comp.spv を要求 — 無ければ false)。
    /// probe grid の寸法は init 時に確定する (途中変更は shutdown → 再 init)。
    bool initialize(VulkanContext& vk, const std::string& shader_dir,
                    uint32_t max_objects, const GIProbeConfig& config);

    /// gi_intensity / max_probe_distance 等の値のみ更新する (grid 寸法は
    /// init 時のまま — 変えたい場合は再 init)。
    void set_probe_config(const GIProbeConfig& config);

    /// 環境反射 (GIReflectionProbe) のパラメータを UBO へ反映する
    /// (phase 3 — gi.glsl の sampleGIEnvSpecular が読む)。 intensity 0 で
    /// 環境反射は無効 (binding は fallback cubemap を維持)。
    void set_env_params(float intensity, uint32_t mip_count);

    /// probe SH (probe_count × 36 float、 `GIProbeField::sh_data()` 互換) を
    /// GPU バッファへ書く。 確保済み probe 数を超える分は切り捨て。
    void upload_probe_sh(const float* sh_data, uint32_t probe_count);

    /// 今フレームの対象オブジェクト (通常 dynamic pool) を書く。
    /// `visibility` は 1 byte / object (0 = skip)。 nullptr = 全可視。
    void update_objects(const float4x4* transforms, uint32_t count,
                        const uint8_t* visibility = nullptr);

    /// per-object irradiance の compute dispatch を記録する。
    /// 末尾の barrier が compute write → vertex/fragment read を保証する。
    void record(VkCommandBuffer cmd);

    // ---- マテリアル結線用の実バッファ ----

    VkBuffer     params_buffer() const            { return params_.buf; }
    VkDeviceSize params_size() const              { return params_.size; }
    VkBuffer     probe_sh_buffer() const          { return probe_sh_.buf; }
    VkDeviceSize probe_sh_size() const            { return probe_sh_.size; }
    VkBuffer     object_irradiance_buffer() const { return object_irradiance_.buf; }
    VkDeviceSize object_irradiance_size() const   { return object_irradiance_.size; }

    uint32_t object_count() const { return object_count_; }
    uint32_t probe_count() const  { return probe_count_; }
#endif

    void shutdown();

private:
#ifdef PICTOR_HAS_VULKAN
    /// host-visible + coherent の永続マップ付きバッファ。
    struct Buffer {
        VkBuffer       buf    = VK_NULL_HANDLE;
        VkDeviceMemory mem    = VK_NULL_HANDLE;
        void*          mapped = nullptr;
        VkDeviceSize   size   = 0;
    };

    bool create_buffer_(Buffer& out, VkDeviceSize size,
                        VkBufferUsageFlags usage);
    void destroy_buffer_(Buffer& b);
    void write_params_();

    VulkanContext* vk_     = nullptr;
    VkDevice       device_ = VK_NULL_HANDLE;

    GIProbeConfig config_{};
    uint32_t      max_objects_  = 0;
    uint32_t      probe_count_  = 0;
    uint32_t      object_count_ = 0;
    float         env_intensity_ = 0.0f;
    uint32_t      env_mip_count_ = 1;

    Buffer params_;             // GIProbeParams UBO (gi_probe_sample.comp / gi.glsl)
    Buffer transforms_;         // mat4 × max_objects
    Buffer visibility_;         // uint × max_objects
    Buffer probe_sh_;           // vec4 × 9 × probe_count
    Buffer object_irradiance_;  // vec4 × 9 × max_objects

    VkDescriptorSetLayout dsl_       = VK_NULL_HANDLE;
    VkPipelineLayout      layout_    = VK_NULL_HANDLE;
    VkPipeline            pipeline_  = VK_NULL_HANDLE;
    VkDescriptorPool      desc_pool_ = VK_NULL_HANDLE;
    VkDescriptorSet       desc_set_  = VK_NULL_HANDLE;
#endif

    bool initialized_ = false;
};

} // namespace pictor
