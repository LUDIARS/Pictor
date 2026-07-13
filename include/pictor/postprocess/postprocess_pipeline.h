#pragma once

// core/types.h を Vulkan ヘッダより先に取り込む。 Win32 では <vulkan/vulkan.h>
// が <windows.h> 経由で GDI の `TRANSPARENT` / `OPAQUE` 等をマクロ定義する
// ため、 これらと同名の enum / constexpr を持つ core ヘッダは vulkan.h より
// 先に処理する必要がある。
#include "pictor/core/types.h"

#include "pictor/postprocess/postprocess_effect.h"
#include "pictor/postprocess/postprocess_chain.h"
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#ifdef PICTOR_HAS_VULKAN
#include <vulkan/vulkan.h>
#endif

namespace pictor {

class VulkanContext;

/// Real, host-driven post-process pipeline.
///
/// The host renders its 3D scene into the pipeline-owned HDR target
/// (`scene_render_pass()` / `scene_framebuffer()`), then calls `record()`
/// to run the effect chain straight onto a swapchain image.
///
/// 構造 (`spec/rendering-extensibility-design.md` §6.3, phase 2 項目1):
///   旧実装は固定 4-pass (extract → blur H → blur V → grade) をベタ書き
///   していた。 現在は `PostProcessChain` (`PostProcessPassDef[]`) を
///   `build_post_process_chain()` で生成し、 `PostProcessPipeline` は
///   その記述から render pass / ターゲット / descriptor / pipeline を
///   動的に構築する。 組み込み 4 エフェクトは「組み込み pass テンプレート」
///   として同じ汎用チェーンへ畳み込まれ、 SSAO / FXAA 等の任意 pass を
///   挿入できる。
///
/// 既定 (= KS が使う組み込みチェーン) の流れは旧実装と同一:
///   scene(HDR) → bloom extract → blur H → blur V
///              → final composite (bloom + tonemap + LUT + vignette) → output
///
/// disabled なエフェクトは push-constant により恒等へ縮退するため、
/// 中間 image レイアウトは毎フレーム有効を保つ。
class PostProcessPipeline {
public:
    PostProcessPipeline();
    ~PostProcessPipeline();

    PostProcessPipeline(const PostProcessPipeline&) = delete;
    PostProcessPipeline& operator=(const PostProcessPipeline&) = delete;

    // ---- Config ----
    void set_config(const PostProcessConfig& config);
    const PostProcessConfig& config() const { return config_; }
    PostProcessConfig&       config_mut()    { return config_; }   ///< live tweaking

    bool is_initialized() const { return vulkan_ready_; }

#ifdef PICTOR_HAS_VULKAN
    /// Real Vulkan init. `output_views` are the swapchain image views the
    /// final pass renders into. `shader_dir` holds the compiled `.spv` files.
    /// `output_format` is the swapchain format.
    ///
    /// The LUT is supplied as host-decoded RGBA8 pixels (`lut_rgba`,
    /// `lut_w` x `lut_h`); pass nullptr for no LUT. Pictor stays free of any
    /// image-decoding dependency — the host loads `config.color_grading.lut_path`.
    /// Returns false on failure.
    ///
    /// 内部で `build_post_process_chain()` を呼んで組み込みチェーンを
    /// 構築する。 任意 pass を挿入したいホストは `initialize_chain()` を
    /// 使う。
    bool initialize_vulkan(VulkanContext& vk,
                           const std::string& shader_dir,
                           uint32_t width, uint32_t height,
                           VkFormat output_format,
                           const std::vector<VkImageView>& output_views,
                           const PostProcessConfig& config,
                           const unsigned char* lut_rgba = nullptr,
                           int lut_w = 0, int lut_h = 0);

    /// 明示的な `PostProcessChain` を渡す初期化。 SSAO / FXAA 等の任意 pass
    /// を含むチェーンをホストが組んで渡せる。 `initialize_vulkan()` は
    /// 組み込みチェーンを生成して本関数を呼ぶ薄いラッパ。
    bool initialize_chain(VulkanContext& vk,
                          const std::string& shader_dir,
                          uint32_t width, uint32_t height,
                          VkFormat output_format,
                          const std::vector<VkImageView>& output_views,
                          const PostProcessConfig& config,
                          const PostProcessChain& chain,
                          const unsigned char* lut_rgba = nullptr,
                          int lut_w = 0, int lut_h = 0);

    /// Render pass / framebuffer the host renders its 3D scene into.
    /// rp_scene_ は color (RGBA16F) + depth (D32_SFLOAT) の 2 アタッチメント。
    /// ホストの vkCmdBeginRenderPass は clearValueCount=2 を渡すこと
    /// (clearValues[0]=color, clearValues[1]=depthStencil)。
    VkRenderPass  scene_render_pass() const { return rp_scene_; }
    VkFramebuffer scene_framebuffer() const { return fb_scene_; }
    VkExtent2D    extent() const { return extent_; }

    /// シーンカラー HDR ビュー (RGBA16F)。 DecalSystem 等がこれへ合成する。
    VkImageView   scene_color_view() const;
    /// シーンの深度ビュー (D32_SFLOAT)。 DecalSystem 等が read する。
    VkImageView   scene_depth_view() const;
    /// シーンカラー HDR の VkImage (RGBA16F)。 KS の Phase 4 wiring が
    /// `AttachmentRegistry::set_external_attachment` で external attachment
    /// として再 expose する用。
    VkImage       scene_color_image() const;
    /// シーン深度の VkImage (D32_SFLOAT)。 同上。
    VkImage       scene_depth_image() const;

    /// Record the post-process chain into `output_views[output_index]`.
    /// The output image is left in COLOR_ATTACHMENT_OPTIMAL so the host can
    /// draw a HUD on top with a LOAD render pass before presenting.
    void record(VkCommandBuffer cmd, uint32_t output_index, float delta_time);

    /// Recreate size-dependent resources after a swapchain resize.
    /// Render passes / pipelines / descriptor pool / sampler / LUT are kept;
    /// HDR targets, framebuffers and descriptor writes are rebuilt.
    /// `output_views` are the new swapchain image views.
    bool resize(uint32_t width, uint32_t height,
                const std::vector<VkImageView>& output_views);

    /// チェーンの「構造」 をフレーム間で差し替える (パイプライン途中変更)。
    /// pass の挿入 / 削除 / 配線替え (`insert_post_process_pass()` 等) を
    /// 反映するときに呼ぶ。 push constant 値だけの変更なら `config_mut()` +
    /// 毎フレームの `refresh_post_process_chain()` で足りるので不要。
    ///
    /// 内部で GPU idle を待ち、 ターゲット / pipeline / descriptor を作り直す
    /// (render pass / sampler / LUT / layout キャッシュは維持)。 resize と
    /// 同じく scene ターゲットも再生成されるため、 呼出し後は
    /// `scene_framebuffer()` / `scene_color_view()` / `scene_depth_view()` を
    /// 取り直すこと。 `output_views` は現在の swapchain image views。
    /// 失敗時は false (パイプラインは未初期化状態へ倒れる)。
    bool rebuild_chain(const PostProcessChain& chain,
                       const std::vector<VkImageView>& output_views);

    /// 現在のチェーン記述。 コピーして編集 API で組み替え、
    /// `rebuild_chain()` へ渡す用。
    const PostProcessChain& chain() const { return chain_; }
#endif

    void shutdown();

private:
    PostProcessConfig config_;
    uint32_t          width_  = 0;
    uint32_t          height_ = 0;
    bool              vulkan_ready_ = false;

#ifdef PICTOR_HAS_VULKAN
    /// 1 個の名前付き中間 / シーンターゲット (HDR RGBA16F)。
    struct RenderTarget {
        VkImage        image  = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView    view   = VK_NULL_HANDLE;
        VkFramebuffer  fb     = VK_NULL_HANDLE;
        /// このターゲットの実解像度 (縮小ターゲットはフル解像度 / divisor)。
        VkExtent2D     extent = {0, 0};
        // 深度アタッチメント (scene_ のみ使用。 中間ターゲットは VK_NULL_HANDLE)。
        VkImage        depth_image  = VK_NULL_HANDLE;
        VkDeviceMemory depth_memory = VK_NULL_HANDLE;
        VkImageView    depth_view   = VK_NULL_HANDLE;
        // この target を出力先とする pass 用の framebuffer の render pass。
        VkRenderPass   fb_render_pass = VK_NULL_HANDLE;
        bool           has_depth      = false;
    };
    struct Texture {
        VkImage        image  = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView    view   = VK_NULL_HANDLE;
    };

    /// 1 個の汎用 pass を実行するための解決済み Vulkan リソース。
    struct CompiledPass {
        std::string           name;
        VkRenderPass          render_pass = VK_NULL_HANDLE;  ///< 共有 (所有しない)
        VkPipeline            pipeline    = VK_NULL_HANDLE;
        VkPipelineLayout      layout      = VK_NULL_HANDLE;  ///< 共有 (所有しない)
        VkDescriptorSet       desc_set    = VK_NULL_HANDLE;
        uint32_t              input_count = 0;               ///< descriptor binding 数
        int32_t               output_index = -1;             ///< targets_ index / -1=swapchain
        /// targets_ index 列。 負値は特殊入力:
        ///   -1 = LUT / -2 = scene 深度 / -3-n = history_[n]
        std::vector<int32_t>  input_indices;
        std::vector<uint8_t>  push_data;
        uint32_t              push_size = 0;
        /// viewport / scissor 上書き (0 = フル解像度)。 PostProcessPassDef 由来。
        uint32_t              viewport_w = 0;
        uint32_t              viewport_h = 0;
    };

    /// history buffer 1 本 — 前フレームの論理ターゲット内容を持ち越す
    /// persistent image (`__history:<source>__` 入力の解決先)。
    /// record() がフレーム末尾に source → image のコピーを記録する。
    struct HistoryEntry {
        std::string source;              ///< 参照元の論理ターゲット名
        int32_t     source_index = -1;   ///< targets_ index (毎ビルドで解決)
        Texture     tex;                 ///< persistent image (黒クリア初期化)
    };

    bool build_from_chain_(VkFormat output_format,
                           const std::vector<VkImageView>& output_views);
    bool create_scene_render_pass_();
    VkRenderPass get_or_create_inter_render_pass_();
    VkRenderPass get_or_create_output_render_pass_(VkFormat output_format);
    bool create_target_(RenderTarget& rt, VkRenderPass rp, bool with_depth,
                        VkExtent2D extent);
    bool create_targets_();
    bool create_samplers_();
    bool upload_lut_(const unsigned char* rgba, int w, int h);
    bool create_descriptors_();
    void write_descriptor_sets_();
    VkDescriptorSetLayout get_or_create_dsl_(uint32_t input_count);
    VkPipelineLayout      get_or_create_layout_(uint32_t input_count,
                                                uint32_t push_size);
    bool create_pipelines_();
    bool create_output_framebuffers_(const std::vector<VkImageView>& views);
    /// chain の `__history:*__` 入力を走査して history image 群を確保する
    /// (黒クリア + SHADER_READ_ONLY へ遷移済みの状態で返す)。
    bool create_history_textures_();
    /// フレーム末尾: 各 history の source ターゲット → history image の
    /// コピー (barrier 込み) を記録する。
    void record_history_copies_(VkCommandBuffer cmd);
    /// resize / rebuild_chain 共通のチェーン依存リソース破棄 (targets /
    /// output framebuffers / pipelines / descriptor pool / history)。
    /// GPU idle 前提。
    void destroy_chain_resources_();
    VkShaderModule load_shader_(const std::string& path) const;
    uint32_t find_memory_type_(uint32_t filter, VkMemoryPropertyFlags props) const;
    /// 論理ターゲット名 → targets_ index。 不明名は -1。
    int32_t resolve_target_(const std::string& name) const;

    VulkanContext* vk_     = nullptr;
    VkDevice       device_ = VK_NULL_HANDLE;
    VkExtent2D     extent_ = {0, 0};
    bool           output_is_srgb_ = false;
    VkFormat       output_format_  = VK_FORMAT_UNDEFINED;
    std::string    shader_dir_;

    PostProcessChain chain_;

    VkRenderPass rp_scene_  = VK_NULL_HANDLE;  // RGBA16F + D32_SFLOAT, CLEAR
    VkRenderPass rp_inter_  = VK_NULL_HANDLE;  // RGBA16F, DONT_CARE
    VkRenderPass rp_output_ = VK_NULL_HANDLE;  // swapchain format, DONT_CARE

    // 名前付きターゲット集合。 index 0 = scene、 以降は chain の中間ターゲット。
    std::vector<RenderTarget>                targets_;
    std::unordered_map<std::string, int32_t> target_index_;
    int32_t       scene_index_ = -1;
    VkFramebuffer fb_scene_    = VK_NULL_HANDLE;  // alias of targets_[scene].fb

    std::vector<VkFramebuffer> output_fbs_;    // one per swapchain image

    // history buffers (`__history:<target>__` 入力の解決先)。
    std::vector<HistoryEntry> history_;

    Texture   lut_{};
    bool      lut_loaded_ = false;
    VkSampler sampler_ = VK_NULL_HANDLE;
    // 深度入力 (__depth__) 用 NEAREST サンプラ。 D32 は LINEAR filter 対応が
    // 保証されないため color 用の sampler_ と分ける。
    VkSampler depth_sampler_ = VK_NULL_HANDLE;

    VkDescriptorPool desc_pool_ = VK_NULL_HANDLE;
    // input 数 → descriptor set layout。 組み込みチェーンは 1 と 3 を使う。
    std::unordered_map<uint32_t, VkDescriptorSetLayout> dsl_by_input_count_;
    // (input 数, push サイズ) → pipeline layout。
    std::vector<std::pair<uint64_t, VkPipelineLayout>>  layout_cache_;

    std::vector<CompiledPass> compiled_;
#endif
};

} // namespace pictor
