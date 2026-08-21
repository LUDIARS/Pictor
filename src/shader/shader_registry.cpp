#include "pictor/shader/shader_registry.h"

#include <cstddef>
#include <cstdio>
#include <fstream>
#include <utility>

#ifdef PICTOR_HAS_VULKAN
#include "pictor/surface/vulkan_context.h"
#include "pictor/shader/vertex_layout.h"
#include "pictor/shader/graphics_pipeline_builder.h"
#endif

namespace pictor {

#ifdef PICTOR_HAS_VULKAN
namespace {

constexpr std::size_t kMaxShaderModuleBytes = std::size_t{64} << 20;
constexpr std::uint32_t kSpirvMagic = 0x07230203u;
constexpr std::size_t kSpirvHeaderWords = 5;

} // namespace
#endif

ShaderRegistry::~ShaderRegistry() { shutdown(); }

ShaderHandle ShaderRegistry::register_shader(CustomShaderDef def) {
    // phase 1 は固定 vert+frag のみ受け付ける。 compute は phase 2。
    if (def.vert_spv.empty() || def.frag_spv.empty()) {
        std::fprintf(stderr,
                     "[shader] register_shader: vert/frag are both required "
                     "(name=%s)\n", def.name.c_str());
        return INVALID_SHADER;
    }
    const ShaderHandle handle = static_cast<ShaderHandle>(shaders_.size());
    shaders_.push_back(std::move(def));
    return handle;
}

const CustomShaderDef* ShaderRegistry::get(ShaderHandle handle) const {
    if (handle == INVALID_SHADER || handle >= shaders_.size()) return nullptr;
    return &shaders_[handle];
}

#ifdef PICTOR_HAS_VULKAN

VkShaderModule ShaderRegistry::load_module_(const std::string& path) const {
    if (path.find('\0') != std::string::npos) {
        std::fprintf(stderr, "[shader] path contains NUL\n");
        return VK_NULL_HANDLE;
    }
    std::ifstream f(path, std::ios::ate | std::ios::binary);
    if (!f) {
        std::fprintf(stderr, "[shader] open failed: %s\n", path.c_str());
        return VK_NULL_HANDLE;
    }
    const std::streamsize end = f.tellg();
    if (end < static_cast<std::streamsize>(kSpirvHeaderWords * sizeof(std::uint32_t)) ||
        static_cast<std::uintmax_t>(end) > kMaxShaderModuleBytes ||
        end % static_cast<std::streamsize>(sizeof(std::uint32_t)) != 0) {
        std::fprintf(stderr, "[shader] invalid SPIR-V size: %s\n", path.c_str());
        return VK_NULL_HANDLE;
    }
    const size_t sz = static_cast<size_t>(end);
    std::vector<std::uint32_t> words(sz / sizeof(std::uint32_t));
    f.seekg(0);
    if (!f.read(reinterpret_cast<char*>(words.data()),
                static_cast<std::streamsize>(sz))) {
        std::fprintf(stderr, "[shader] read failed: %s\n", path.c_str());
        return VK_NULL_HANDLE;
    }
    if (words[0] != kSpirvMagic) {
        std::fprintf(stderr, "[shader] invalid SPIR-V magic: %s\n", path.c_str());
        return VK_NULL_HANDLE;
    }

    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = sz;
    ci.pCode    = words.data();
    VkShaderModule m = VK_NULL_HANDLE;
    if (vkCreateShaderModule(device_, &ci, nullptr, &m) != VK_SUCCESS) {
        std::fprintf(stderr, "[shader] vkCreateShaderModule failed: %s\n",
                     path.c_str());
        return VK_NULL_HANDLE;
    }
    return m;
}

bool ShaderRegistry::build_pipelines(VulkanContext& vk,
                                     VkRenderPass     render_pass,
                                     uint32_t         subpass,
                                     VkPipelineLayout pipeline_layout) {
    if (built_) return true;
    if (shaders_.empty()) {
        // 空 build でも context / 引数は記憶する。 後から register された
        // シェーダを rebuild_pipelines() が実 build できるようにするため。
        vk_     = &vk;
        device_ = vk.device();
        built_render_pass_ = render_pass;
        built_subpass_     = subpass;
        built_layout_      = pipeline_layout;
        built_ = true;
        return true;
    }
    if (render_pass == VK_NULL_HANDLE || pipeline_layout == VK_NULL_HANDLE) {
        std::fprintf(stderr, "[shader] build_pipelines: render pass / layout "
                             "must be valid\n");
        return false;
    }

    vk_     = &vk;
    device_ = vk.device();
    built_render_pass_ = render_pass;
    built_subpass_     = subpass;
    built_layout_      = pipeline_layout;

    // 初回 build: 失敗したシェーダは VK_NULL_HANDLE のまま警告してスキップ
    // する (部分成功を許す)。 all-or-nothing の差し替えは rebuild 側の責務。
    std::vector<VkPipeline> candidates;
    const bool all_ok = build_candidates_(render_pass, subpass,
                                          pipeline_layout, candidates);
    // invalidate_build_target() → build_pipelines() の再 build 経路では旧
    // pipeline 群がまだ生きている (invalidate は built_ を戻すだけで破棄
    // しない)。 上書きすると handle を失うので、 提出済み command buffer の
    // 参照が切れるのを待ってから破棄する。
    if (!pipelines_.empty()) {
        vkDeviceWaitIdle(device_);
        for (VkPipeline p : pipelines_) {
            if (p != VK_NULL_HANDLE) vkDestroyPipeline(device_, p, nullptr);
        }
    }
    pipelines_ = std::move(candidates);
    built_ = true;
    return all_ok;
}

bool ShaderRegistry::build_candidates_(VkRenderPass     render_pass,
                                       uint32_t         subpass,
                                       VkPipelineLayout pipeline_layout,
                                       std::vector<VkPipeline>& out) {
    out.assign(shaders_.size(), VK_NULL_HANDLE);

    // graphics pipeline 生成は共通ヘルパー `build_graphics_pipeline()` に
    // 委譲する (§6.3 項目6 — PostProcessPipeline との重複統合)。
    // カスタムシェーダ pipeline は mesh 用なので背面カリング + 深度テスト
    // 有効、 頂点入力は CustomShaderDef::vertex_layout から構築する。
    // 空レイアウトのときは頂点入力空 = phase 1 互換 (gl_VertexIndex 駆動)。
    bool all_ok = true;
    for (size_t i = 0; i < shaders_.size(); ++i) {
        const CustomShaderDef& def = shaders_[i];

        VkShaderModule vs = load_module_(def.vert_spv);
        VkShaderModule fs = VK_NULL_HANDLE;
        if (vs) fs = load_module_(def.frag_spv);
        if (!vs || !fs) {
            if (vs) vkDestroyShaderModule(device_, vs, nullptr);
            std::fprintf(stderr, "[shader] pipeline skipped (module load "
                                 "failed): %s\n", def.name.c_str());
            all_ok = false;
            continue;
        }

        GraphicsPipelineDesc gd;
        gd.vert          = vs;
        gd.frag          = fs;
        gd.render_pass   = render_pass;
        gd.subpass       = subpass;
        gd.layout        = pipeline_layout;
        gd.vertex_layout = def.vertex_layout;
        gd.cull_back     = true;   // mesh — 背面カリング
        gd.depth_test    = true;   // mesh — 深度テスト

        const bool ok = build_graphics_pipeline(device_, gd, out[i]);
        vkDestroyShaderModule(device_, vs, nullptr);
        vkDestroyShaderModule(device_, fs, nullptr);
        if (!ok) {
            std::fprintf(stderr, "[shader] vkCreateGraphicsPipelines failed: "
                                 "%s\n", def.name.c_str());
            out[i] = VK_NULL_HANDLE;
            all_ok = false;
        }
    }

    return all_ok;
}

VkPipeline ShaderRegistry::pipeline(ShaderHandle handle) const {
    if (!built_ || handle == INVALID_SHADER || handle >= pipelines_.size())
        return VK_NULL_HANDLE;
    return pipelines_[handle];
}

void ShaderRegistry::invalidate_build_target() {
    // 記憶した handle は破棄後も non-null のままなので、 dangling を
    // Pictor 側から検知する手段が無い。 ホストが破棄前にここを呼ぶことで
    // 「記憶なし」 へ戻し、 rebuild_pipelines() が古い render pass /
    // layout を使って未定義動作に入るのを防ぐ。
    built_render_pass_ = VK_NULL_HANDLE;
    built_subpass_     = 0;
    built_layout_      = VK_NULL_HANDLE;
    // build target を失った以上 build 済みとは言えない。 ホストが新しい
    // render pass で build_pipelines() を呼び直せるよう false へ戻す
    // (build_pipelines は built_ のとき即 true で戻るため)。
    built_ = false;
}

#endif // PICTOR_HAS_VULKAN

bool ShaderRegistry::rebuild_pipelines() {
#ifdef PICTOR_HAS_VULKAN
    if (!built_) return true;   // まだ build していない — リロード対象なし
    if (!vk_ || device_ == VK_NULL_HANDLE) {
        // built_ なのに context 未記憶 — build-after-register の契約外。
        // 「何も build せず成功」を返すと呼び出し側が回復不能になるため
        // 失敗として報告する。
        std::fprintf(stderr, "[shader] rebuild_pipelines: no recorded build "
                             "context\n");
        return false;
    }
    if (shaders_.empty()) return true;   // リロード対象なし

    if (built_render_pass_ == VK_NULL_HANDLE ||
        built_layout_ == VK_NULL_HANDLE) {
        std::fprintf(stderr, "[shader] rebuild_pipelines: recorded render "
                             "pass / layout are invalid\n");
        return false;
    }

    // 候補 pipeline はローカル vector へ build し、 現行 pipelines_ には
    // 一切触れない。 壊れた / 書き込み途中の SPIR-V が 1 本でもあれば候補
    // 全体を破棄して false — last-known-good の描画経路はそのまま残る
    // (shader_registry.h / pipeline-hot-reload.md の契約)。
    std::vector<VkPipeline> candidates;
    if (!build_candidates_(built_render_pass_, built_subpass_, built_layout_,
                           candidates)) {
        // 候補はまだどの command buffer にも bind されていないため、
        // device idle を待たずに破棄してよい。
        for (VkPipeline p : candidates) {
            if (p != VK_NULL_HANDLE) vkDestroyPipeline(device_, p, nullptr);
        }
        return false;
    }

    // 全件成功時のみ差し替える。 提出済み command buffer が旧 pipeline を
    // 参照している可能性があるため、 破棄前に device idle を待つ。
    vkDeviceWaitIdle(device_);
    for (VkPipeline p : pipelines_) {
        if (p != VK_NULL_HANDLE) vkDestroyPipeline(device_, p, nullptr);
    }
    pipelines_ = std::move(candidates);
#endif
    return true;
}

void ShaderRegistry::shutdown() {
#ifdef PICTOR_HAS_VULKAN
    if (device_ != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device_);
        for (VkPipeline p : pipelines_) {
            if (p != VK_NULL_HANDLE) vkDestroyPipeline(device_, p, nullptr);
        }
        device_ = VK_NULL_HANDLE;
    }
    pipelines_.clear();
    vk_    = nullptr;
    built_ = false;
    // 記憶した build target も捨てる。 shutdown 後に再 build せず
    // rebuild_pipelines() を呼ばれても、 破棄済み render pass / layout を
    // 掴んだままにしない。
    built_render_pass_ = VK_NULL_HANDLE;
    built_subpass_     = 0;
    built_layout_      = VK_NULL_HANDLE;
#endif
}

} // namespace pictor
