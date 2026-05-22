#include "pictor/shader/shader_registry.h"

#include <cstdio>
#include <fstream>
#include <utility>

#ifdef PICTOR_HAS_VULKAN
#include "pictor/surface/vulkan_context.h"
#include "pictor/shader/vertex_layout.h"
#include "pictor/shader/graphics_pipeline_builder.h"
#endif

namespace pictor {

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
    std::ifstream f(path, std::ios::ate | std::ios::binary);
    if (!f) {
        std::fprintf(stderr, "[shader] open failed: %s\n", path.c_str());
        return VK_NULL_HANDLE;
    }
    const size_t sz = static_cast<size_t>(f.tellg());
    std::vector<char> buf(sz);
    f.seekg(0);
    f.read(buf.data(), static_cast<std::streamsize>(sz));

    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = sz;
    ci.pCode    = reinterpret_cast<const uint32_t*>(buf.data());
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
