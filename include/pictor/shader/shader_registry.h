#pragma once

/// ShaderRegistry — Visus CUSTOM kind 用の最小シェーダ登録 + pipeline 生成経路.
///
/// 背景 (`spec/rendering-extensibility-design.md` §2 phase 1):
///   Pictor のレンダリングは固定 PBR マテリアル層 (系統A 寄り) と、
///   各所でハードコードされた `.spv` 読み込み (系統B) に分かれていた。
///   Visus の CUSTOM kind は「固定 vert+frag のカスタムシェーダを選んで
///   描画に反映する」 ための seam だが、 シェーダ → `VkPipeline` の共通
///   ロード経路が無かった。
///
/// 本クラスはその欠けていた経路。 `PostProcessPipeline::create_pipelines_`
/// の graphics pipeline 生成コードを雛形に、 CUSTOM kind の Visus が
/// 指す vert+frag SPIR-V から 1 本の `VkPipeline` を作って登録する。
///
/// スコープ (phase 1):
///   - 固定 vert+frag の graphics pipeline のみ。 compute は phase 2。
///   - permutation / 任意 uniform バインドは phase 2。 descriptor set
///     layout は「64-byte push constant のみ」 の最小構成で固定する。
///   - 固定 PBR マテリアル層は温存。 CUSTOM kind の object だけが
///     `ObjectDescriptor::customShader` 経由でこの pipeline を使う。
///
/// `PICTOR_HAS_VULKAN` 無効時はハンドル管理だけ行い、 pipeline は作らない
/// (ヘッドレステスト用)。

#include "pictor/core/types.h"

#include <cstdint>
#include <string>
#include <vector>

#ifdef PICTOR_HAS_VULKAN
#include <vulkan/vulkan.h>
#endif

namespace pictor {

class VulkanContext;

/// 1 個のカスタムシェーダ登録。 `vert_spv` / `frag_spv` は compiled
/// SPIR-V ファイルへのパス (Visus の `VisusShaderRef` STAGES 形式
/// = `metadata["shader"]` の `vert` / `frag` から解決済み)。
struct CustomShaderDef {
    std::string name;        ///< 表示名 / デバッグ用 (Visus name など)
    std::string vert_spv;    ///< vertex stage SPIR-V のパス
    std::string frag_spv;    ///< fragment stage SPIR-V のパス

    /// mesh 駆動の頂点入力レイアウト
    /// (`spec/rendering-extensibility-design.md` §6.2 phase 2)。
    /// 空 (`vertex_layout.empty()`) のとき pipeline は頂点入力空で作られ、
    /// phase 1 互換の `gl_VertexIndex` 駆動カスタムシェーダになる。
    /// 非空なら mesh の頂点バッファ (binding 0) を読む pipeline になる。
    VertexLayout vertex_layout;
};

/// Visus CUSTOM kind 用の最小シェーダレジストリ。
///
/// 使い方:
///   1. `register_shader()` で CustomShaderDef を登録 → `ShaderHandle` 取得。
///      Visus v2 は JSON に handle を持たないので (`spec/feature/visus-v2-design.md`
///      §2.1)、 この handle は実行時 side-table (VisusRuntime) が
///      `metadata["shader"]` の解決結果として保持する。
///   2. (Vulkan 有効時) `build_pipelines()` を描画ターゲットの render pass
///      が確定したあとに 1 度呼ぶ。 登録済み全シェーダの `VkPipeline` を
///      生成する。
///   3. レンダラは `ObjectDescriptor::customShader` (または
///      `ShaderKey::custom_shader(shaderKey)`) から `ShaderHandle` を得て
///      `pipeline(handle)` で `VkPipeline` を引く。
class ShaderRegistry {
public:
    ShaderRegistry() = default;
    ~ShaderRegistry();

    ShaderRegistry(const ShaderRegistry&)            = delete;
    ShaderRegistry& operator=(const ShaderRegistry&) = delete;

    /// カスタムシェーダを登録する。 戻り値は登録順の連番ハンドル。
    /// vert / frag のいずれかが空なら `INVALID_SHADER` を返す
    /// (phase 1 は固定 vert+frag のみ受け付ける)。
    ShaderHandle register_shader(CustomShaderDef def);

    /// handle から定義を引く。 範囲外なら nullptr。
    const CustomShaderDef* get(ShaderHandle handle) const;

    /// 登録済みシェーダ数。
    size_t count() const { return shaders_.size(); }

#ifdef PICTOR_HAS_VULKAN
    /// 登録済み全シェーダの `VkPipeline` を生成する。
    ///
    /// `render_pass` / `subpass` は CUSTOM kind の object を描く対象パス。
    /// `pipeline_layout` は呼び出し側が用意する (CUSTOM kind 共通の
    /// descriptor set layout + 64-byte push constant range を持つ前提)。
    /// `register_shader()` 後・描画開始前に 1 度呼ぶ。
    ///
    /// 1 本でも生成に失敗したら false。 既に build 済みなら何もせず true。
    bool build_pipelines(VulkanContext& vk,
                         VkRenderPass     render_pass,
                         uint32_t         subpass,
                         VkPipelineLayout pipeline_layout);

    /// handle に対応する `VkPipeline`。 未 build / 範囲外なら VK_NULL_HANDLE。
    VkPipeline pipeline(ShaderHandle handle) const;

    bool is_built() const { return built_; }
#endif

    /// 全 pipeline / shader module を破棄する。 登録定義は残す。
    void shutdown();

private:
    std::vector<CustomShaderDef> shaders_;

#ifdef PICTOR_HAS_VULKAN
    VkShaderModule load_module_(const std::string& path) const;

    VulkanContext*          vk_     = nullptr;
    VkDevice                device_ = VK_NULL_HANDLE;
    std::vector<VkPipeline> pipelines_;
    bool                    built_  = false;
#endif
};

} // namespace pictor
