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
    if (end <= 0 || static_cast<std::uintmax_t>(end) > kMaxShaderModuleBytes ||
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
    if (shaders_.empty()) { built_ = true; return true; }
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
    pipelines_.assign(shaders_.size(), VK_NULL_HANDLE);

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

        const bool ok = build_graphics_pipeline(device_, gd, pipelines_[i]);
        vkDestroyShaderModule(device_, vs, nullptr);
        vkDestroyShaderModule(device_, fs, nullptr);
        if (!ok) {
            std::fprintf(stderr, "[shader] vkCreateGraphicsPipelines failed: "
                                 "%s\n", def.name.c_str());
            pipelines_[i] = VK_NULL_HANDLE;
            all_ok = false;
        }
    }

    built_ = true;
    return all_ok;
}

VkPipeline ShaderRegistry::pipeline(ShaderHandle handle) const {
    if (!built_ || handle == INVALID_SHADER || handle >= pipelines_.size())
        return VK_NULL_HANDLE;
    return pipelines_[handle];
}

#endif // PICTOR_HAS_VULKAN

bool ShaderRegistry::rebuild_pipelines() {
#ifdef PICTOR_HAS_VULKAN
    if (!built_) return true;   // まだ build していない — リロード対象なし
    if (!vk_ || device_ == VK_NULL_HANDLE) return true;   // 空 build (shaders_ 空) だった

    VulkanContext&   vk          = *vk_;
    VkRenderPass     render_pass = built_render_pass_;
    uint32_t         subpass     = built_subpass_;
    VkPipelineLayout layout      = built_layout_;

    // Candidate pipelines are built while the current set remains live. A
    // malformed or half-written SPIR-V file therefore cannot destroy the
    // last-known-good render path.
    std::vector<VkPipeline> old_pipelines = std::move(pipelines_);
    built_ = false;
    if (!build_pipelines(vk, render_pass, subpass, layout)) {
        for (VkPipeline p : pipelines_) {
            if (p != VK_NULL_HANDLE) vkDestroyPipeline(device_, p, nullptr);
        }
        pipelines_ = std::move(old_pipelines);
        built_ = true;
        return false;
    }

    // Only the successful candidate set is published. Wait before destroying
    // pipelines that may still be referenced by submitted command buffers.
    vkDeviceWaitIdle(device_);
    for (VkPipeline p : old_pipelines) {
        if (p != VK_NULL_HANDLE) vkDestroyPipeline(device_, p, nullptr);
    }
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
#endif
}

} // namespace pictor
