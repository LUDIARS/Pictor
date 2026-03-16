/// Pictor Text Rendering Demo
///
/// Demonstrates two text rendering approaches:
///
/// **Mode 1 — SVG Vector Text** (Left/Right to switch)
///   Renders "あ" and "薔薇" at multiple sizes (16px → 256px) using
///   TextSvgRenderer → TextImageRenderer. Because glyph outlines are
///   re-rasterized from vector data at each target size, there is zero
///   quality degradation at any resolution.
///
/// **Mode 2 — Rasterized Text with Effects**
///   Renders free-format text using TextImageRenderer with GPU-side
///   effects: outline, drop shadow, and glow — applied in real-time
///   via a fragment shader.
///
/// Controls:
///   Left / Right arrow : switch demo mode
///   1–4                : cycle through effects (none / outline / shadow / glow)
///   ESC                : quit
///
/// Build target: pictor_text_demo

#include "pictor/pictor.h"
#include "pictor/surface/vulkan_context.h"
#include "pictor/surface/glfw_surface_provider.h"
#include "pictor/text/font_loader.h"
#include "pictor/text/text_svg_renderer.h"
#include "pictor/text/text_image_renderer.h"
#include "pictor/text/text_rasterizer.h"

#include <GLFW/glfw3.h>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <chrono>
#include <vector>
#include <string>
#include <algorithm>
#include <array>

using namespace pictor;

// ============================================================
// Demo state
// ============================================================

static int g_demo_mode  = 0; // 0 = SVG, 1 = Rasterized
static int g_effect     = 0; // 0 = none, 1 = outline, 2 = shadow, 3 = glow
static bool g_needs_rebuild = true;

static const char* mode_names[]   = {"SVG Vector Text", "Rasterized Text + Effects"};
static const char* effect_names[] = {"No Effect", "Outline", "Drop Shadow", "Glow"};

// ============================================================
// Push constants (must match text_quad.frag)
// ============================================================

struct TextPushConstants {
    int32_t  render_mode;   // 0 = textured, 1 = solid color
    int32_t  effect_mode;   // 0..3
    float    outline_color[4];
    float    outline_width;
    float    _pad[3];       // align to 16 bytes
};

// ============================================================
// Quad vertex (must match text_quad.vert)
// ============================================================

struct QuadVertex {
    float pos[2];
    float uv[2];
    float color[4];
};

// ============================================================
// Textured quad entry: a single image displayed on screen
// ============================================================

struct TexturedQuad {
    // Screen-space pixel rect
    float x, y, w, h;
    // RGBA image data (CPU side, uploaded to a Vulkan texture)
    ImageBuffer image;
};

// ============================================================
// Forward declarations — Vulkan helpers
// ============================================================

#ifdef PICTOR_HAS_VULKAN

class TextDemoRenderer {
public:
    bool initialize(VulkanContext& vk_ctx, const char* shader_dir);
    void shutdown();
    ~TextDemoRenderer() { shutdown(); }

    /// Prepare quads for rendering. Uploads textures, generates vertices.
    void set_quads(const std::vector<TexturedQuad>& quads);

    /// Record draw commands.
    void render(VkCommandBuffer cmd, VkRenderPass render_pass,
                VkFramebuffer framebuffer, VkExtent2D extent,
                const TextPushConstants& pc);

private:
    bool create_buffer(VkDeviceSize size, VkBufferUsageFlags usage,
                       VkMemoryPropertyFlags props, VkBuffer& buf, VkDeviceMemory& mem);
    uint32_t find_memory_type(uint32_t type_filter, VkMemoryPropertyFlags props);
    VkShaderModule load_shader(const char* path);
    bool create_descriptor_layout();
    bool create_pipeline(const char* shader_dir);
    bool create_sampler();

    // Per-quad texture
    struct QuadTexture {
        VkImage        image       = VK_NULL_HANDLE;
        VkDeviceMemory memory      = VK_NULL_HANDLE;
        VkImageView    view        = VK_NULL_HANDLE;
        VkDescriptorSet desc_set   = VK_NULL_HANDLE;
        uint32_t       width  = 0;
        uint32_t       height = 0;
    };

    void destroy_quad_textures();
    bool upload_texture(const ImageBuffer& img, QuadTexture& out);

    /// Release partially initialized resources (when initialize fails mid-way)
    void shutdown_partial() {
        if (!device_) return;
        vkDeviceWaitIdle(device_);
        if (sampler_)         { vkDestroySampler(device_, sampler_, nullptr); sampler_ = VK_NULL_HANDLE; }
        if (pipeline_)        { vkDestroyPipeline(device_, pipeline_, nullptr); pipeline_ = VK_NULL_HANDLE; }
        if (pipeline_layout_) { vkDestroyPipelineLayout(device_, pipeline_layout_, nullptr); pipeline_layout_ = VK_NULL_HANDLE; }
        if (desc_set_layout_) { vkDestroyDescriptorSetLayout(device_, desc_set_layout_, nullptr); desc_set_layout_ = VK_NULL_HANDLE; }
        if (desc_pool_)       { vkDestroyDescriptorPool(device_, desc_pool_, nullptr); desc_pool_ = VK_NULL_HANDLE; }
    }

    VulkanContext* vk_ctx_ = nullptr;
    VkDevice device_ = VK_NULL_HANDLE;

    VkPipelineLayout pipeline_layout_       = VK_NULL_HANDLE;
    VkPipeline pipeline_                    = VK_NULL_HANDLE;
    VkDescriptorSetLayout desc_set_layout_  = VK_NULL_HANDLE;
    VkDescriptorPool desc_pool_             = VK_NULL_HANDLE;
    VkSampler sampler_                      = VK_NULL_HANDLE;

    VkBuffer vertex_buffer_          = VK_NULL_HANDLE;
    VkDeviceMemory vertex_memory_    = VK_NULL_HANDLE;
    uint32_t vertex_count_           = 0;

    std::vector<QuadTexture> textures_;

    // Per-quad info for draw calls
    struct DrawEntry {
        uint32_t first_vertex;
        uint32_t vertex_count;
        VkDescriptorSet desc_set;
    };
    std::vector<DrawEntry> draw_entries_;

    bool initialized_ = false;
};

// ============================================================
// TextDemoRenderer implementation
// ============================================================

uint32_t TextDemoRenderer::find_memory_type(uint32_t type_filter, VkMemoryPropertyFlags props) {
    VkPhysicalDeviceMemoryProperties mem_props;
    vkGetPhysicalDeviceMemoryProperties(vk_ctx_->physical_device(), &mem_props);
    for (uint32_t i = 0; i < mem_props.memoryTypeCount; i++) {
        if ((type_filter & (1 << i)) &&
            (mem_props.memoryTypes[i].propertyFlags & props) == props)
            return i;
    }
    return 0;
}

bool TextDemoRenderer::create_buffer(VkDeviceSize size, VkBufferUsageFlags usage,
                                      VkMemoryPropertyFlags props,
                                      VkBuffer& buf, VkDeviceMemory& mem) {
    VkBufferCreateInfo info{};
    info.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.size        = size;
    info.usage       = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(device_, &info, nullptr, &buf) != VK_SUCCESS) return false;

    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(device_, buf, &req);

    VkMemoryAllocateInfo alloc{};
    alloc.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize  = req.size;
    alloc.memoryTypeIndex = find_memory_type(req.memoryTypeBits, props);
    if (vkAllocateMemory(device_, &alloc, nullptr, &mem) != VK_SUCCESS) return false;
    vkBindBufferMemory(device_, buf, mem, 0);
    return true;
}

VkShaderModule TextDemoRenderer::load_shader(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "Cannot open shader: %s\n", path); return VK_NULL_HANDLE; }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    std::vector<char> code(len);
    fread(code.data(), 1, len, f);
    fclose(f);

    VkShaderModuleCreateInfo info{};
    info.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    info.codeSize = code.size();
    info.pCode    = reinterpret_cast<const uint32_t*>(code.data());

    VkShaderModule mod;
    if (vkCreateShaderModule(device_, &info, nullptr, &mod) != VK_SUCCESS)
        return VK_NULL_HANDLE;
    return mod;
}

bool TextDemoRenderer::create_descriptor_layout() {
    // binding 0: combined image sampler
    VkDescriptorSetLayoutBinding binding{};
    binding.binding         = 0;
    binding.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = 1;
    binding.stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo info{};
    info.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    info.bindingCount = 1;
    info.pBindings    = &binding;

    return vkCreateDescriptorSetLayout(device_, &info, nullptr, &desc_set_layout_) == VK_SUCCESS;
}

bool TextDemoRenderer::create_sampler() {
    VkSamplerCreateInfo info{};
    info.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    info.magFilter    = VK_FILTER_LINEAR;
    info.minFilter    = VK_FILTER_LINEAR;
    info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    info.maxAnisotropy = 1.0f;
    info.borderColor   = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    info.mipmapMode    = VK_SAMPLER_MIPMAP_MODE_LINEAR;

    return vkCreateSampler(device_, &info, nullptr, &sampler_) == VK_SUCCESS;
}

bool TextDemoRenderer::create_pipeline(const char* shader_dir) {
    std::string vert_path = std::string(shader_dir) + "/text_quad.vert.spv";
    std::string frag_path = std::string(shader_dir) + "/text_quad.frag.spv";

    VkShaderModule vert_mod = load_shader(vert_path.c_str());
    VkShaderModule frag_mod = load_shader(frag_path.c_str());
    if (!vert_mod || !frag_mod) {
        fprintf(stderr, "TextDemoRenderer: failed to load shaders from %s\n", shader_dir);
        if (vert_mod) vkDestroyShaderModule(device_, vert_mod, nullptr);
        if (frag_mod) vkDestroyShaderModule(device_, frag_mod, nullptr);
        return false;
    }

    // Push constant range
    VkPushConstantRange pc_range{};
    pc_range.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    pc_range.offset     = 0;
    pc_range.size       = sizeof(TextPushConstants);

    VkPipelineLayoutCreateInfo layout_info{};
    layout_info.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout_info.setLayoutCount         = 1;
    layout_info.pSetLayouts            = &desc_set_layout_;
    layout_info.pushConstantRangeCount = 1;
    layout_info.pPushConstantRanges    = &pc_range;

    if (vkCreatePipelineLayout(device_, &layout_info, nullptr, &pipeline_layout_) != VK_SUCCESS) {
        vkDestroyShaderModule(device_, vert_mod, nullptr);
        vkDestroyShaderModule(device_, frag_mod, nullptr);
        return false;
    }

    // Shader stages
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vert_mod;
    stages[0].pName  = "main";
    stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = frag_mod;
    stages[1].pName  = "main";

    // Vertex input: pos(2) + uv(2) + color(4)
    VkVertexInputBindingDescription bind{};
    bind.binding   = 0;
    bind.stride    = sizeof(QuadVertex);
    bind.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    VkVertexInputAttributeDescription attrs[3]{};
    attrs[0].location = 0; attrs[0].binding = 0;
    attrs[0].format   = VK_FORMAT_R32G32_SFLOAT;
    attrs[0].offset   = offsetof(QuadVertex, pos);
    attrs[1].location = 1; attrs[1].binding = 0;
    attrs[1].format   = VK_FORMAT_R32G32_SFLOAT;
    attrs[1].offset   = offsetof(QuadVertex, uv);
    attrs[2].location = 2; attrs[2].binding = 0;
    attrs[2].format   = VK_FORMAT_R32G32B32A32_SFLOAT;
    attrs[2].offset   = offsetof(QuadVertex, color);

    VkPipelineVertexInputStateCreateInfo vertex_input{};
    vertex_input.sType                           = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertex_input.vertexBindingDescriptionCount   = 1;
    vertex_input.pVertexBindingDescriptions      = &bind;
    vertex_input.vertexAttributeDescriptionCount = 3;
    vertex_input.pVertexAttributeDescriptions    = attrs;

    VkPipelineInputAssemblyStateCreateInfo ia{};
    ia.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo vp{};
    vp.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.viewportCount = 1;
    vp.scissorCount  = 1;

    VkPipelineRasterizationStateCreateInfo rast{};
    rast.sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rast.polygonMode = VK_POLYGON_MODE_FILL;
    rast.cullMode    = VK_CULL_MODE_NONE;
    rast.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rast.lineWidth   = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    // Alpha blending
    VkPipelineColorBlendAttachmentState blend_att{};
    blend_att.blendEnable         = VK_TRUE;
    blend_att.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    blend_att.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend_att.colorBlendOp        = VK_BLEND_OP_ADD;
    blend_att.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blend_att.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend_att.alphaBlendOp        = VK_BLEND_OP_ADD;
    blend_att.colorWriteMask      = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                    VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendStateCreateInfo blend{};
    blend.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend.attachmentCount = 1;
    blend.pAttachments    = &blend_att;

    VkPipelineDepthStencilStateCreateInfo ds{};
    ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;

    VkDynamicState dyn_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dyn{};
    dyn.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dyn.dynamicStateCount = 2;
    dyn.pDynamicStates    = dyn_states;

    VkGraphicsPipelineCreateInfo pipe_info{};
    pipe_info.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipe_info.stageCount          = 2;
    pipe_info.pStages             = stages;
    pipe_info.pVertexInputState   = &vertex_input;
    pipe_info.pInputAssemblyState = &ia;
    pipe_info.pViewportState      = &vp;
    pipe_info.pRasterizationState = &rast;
    pipe_info.pMultisampleState   = &ms;
    pipe_info.pColorBlendState    = &blend;
    pipe_info.pDepthStencilState  = &ds;
    pipe_info.pDynamicState       = &dyn;
    pipe_info.layout              = pipeline_layout_;
    pipe_info.renderPass          = vk_ctx_->default_render_pass();
    pipe_info.subpass             = 0;

    VkResult result = vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1,
                                                 &pipe_info, nullptr, &pipeline_);

    vkDestroyShaderModule(device_, vert_mod, nullptr);
    vkDestroyShaderModule(device_, frag_mod, nullptr);

    return result == VK_SUCCESS;
}

bool TextDemoRenderer::upload_texture(const ImageBuffer& img, QuadTexture& out) {
    out.width  = img.width;
    out.height = img.height;

    // Ensure RGBA
    const uint8_t* pixels = img.pixels.data();
    std::vector<uint8_t> rgba_buf;
    if (img.channels == 1) {
        rgba_buf.resize(img.width * img.height * 4);
        for (uint32_t i = 0; i < img.width * img.height; ++i) {
            rgba_buf[i*4+0] = 255;
            rgba_buf[i*4+1] = 255;
            rgba_buf[i*4+2] = 255;
            rgba_buf[i*4+3] = img.pixels[i];
        }
        pixels = rgba_buf.data();
    }

    VkDeviceSize image_size = static_cast<VkDeviceSize>(img.width) * img.height * 4;

    // Staging buffer
    VkBuffer staging_buf;
    VkDeviceMemory staging_mem;
    if (!create_buffer(image_size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                       staging_buf, staging_mem))
        return false;

    void* mapped;
    vkMapMemory(device_, staging_mem, 0, image_size, 0, &mapped);
    memcpy(mapped, pixels, image_size);
    vkUnmapMemory(device_, staging_mem);

    // Create image
    VkImageCreateInfo img_info{};
    img_info.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    img_info.imageType     = VK_IMAGE_TYPE_2D;
    img_info.format        = VK_FORMAT_R8G8B8A8_UNORM;
    img_info.extent        = {img.width, img.height, 1};
    img_info.mipLevels     = 1;
    img_info.arrayLayers   = 1;
    img_info.samples       = VK_SAMPLE_COUNT_1_BIT;
    img_info.tiling        = VK_IMAGE_TILING_OPTIMAL;
    img_info.usage         = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    img_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    if (vkCreateImage(device_, &img_info, nullptr, &out.image) != VK_SUCCESS) {
        vkDestroyBuffer(device_, staging_buf, nullptr);
        vkFreeMemory(device_, staging_mem, nullptr);
        return false;
    }

    VkMemoryRequirements mem_req;
    vkGetImageMemoryRequirements(device_, out.image, &mem_req);

    VkMemoryAllocateInfo alloc{};
    alloc.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize  = mem_req.size;
    alloc.memoryTypeIndex = find_memory_type(mem_req.memoryTypeBits,
                                              VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (vkAllocateMemory(device_, &alloc, nullptr, &out.memory) != VK_SUCCESS) {
        vkDestroyImage(device_, out.image, nullptr);
        vkDestroyBuffer(device_, staging_buf, nullptr);
        vkFreeMemory(device_, staging_mem, nullptr);
        return false;
    }
    vkBindImageMemory(device_, out.image, out.memory, 0);

    // Copy staging → image via one-shot command buffer
    VkCommandBuffer cmd;
    VkCommandBufferAllocateInfo cmd_alloc{};
    cmd_alloc.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cmd_alloc.commandPool        = vk_ctx_->command_pool();
    cmd_alloc.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmd_alloc.commandBufferCount = 1;
    vkAllocateCommandBuffers(device_, &cmd_alloc, &cmd);

    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &begin);

    // Transition UNDEFINED → TRANSFER_DST
    VkImageMemoryBarrier barrier{};
    barrier.sType                       = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout                   = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout                   = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcAccessMask               = 0;
    barrier.dstAccessMask               = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.image                       = out.image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                         0, nullptr, 0, nullptr, 1, &barrier);

    VkBufferImageCopy region{};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent                 = {img.width, img.height, 1};
    vkCmdCopyBufferToImage(cmd, staging_buf, out.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    // Transition TRANSFER_DST → SHADER_READ_ONLY
    barrier.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
                         0, nullptr, 0, nullptr, 1, &barrier);

    vkEndCommandBuffer(cmd);

    VkSubmitInfo submit{};
    submit.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers    = &cmd;
    vkQueueSubmit(vk_ctx_->graphics_queue(), 1, &submit, VK_NULL_HANDLE);
    vkQueueWaitIdle(vk_ctx_->graphics_queue());

    vkFreeCommandBuffers(device_, vk_ctx_->command_pool(), 1, &cmd);
    vkDestroyBuffer(device_, staging_buf, nullptr);
    vkFreeMemory(device_, staging_mem, nullptr);

    // Image view
    VkImageViewCreateInfo view_info{};
    view_info.sType                           = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.image                           = out.image;
    view_info.viewType                        = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format                          = VK_FORMAT_R8G8B8A8_UNORM;
    view_info.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
    view_info.subresourceRange.levelCount     = 1;
    view_info.subresourceRange.layerCount     = 1;
    if (vkCreateImageView(device_, &view_info, nullptr, &out.view) != VK_SUCCESS)
        return false;

    // Descriptor set
    VkDescriptorSetAllocateInfo ds_alloc{};
    ds_alloc.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    ds_alloc.descriptorPool     = desc_pool_;
    ds_alloc.descriptorSetCount = 1;
    ds_alloc.pSetLayouts        = &desc_set_layout_;
    if (vkAllocateDescriptorSets(device_, &ds_alloc, &out.desc_set) != VK_SUCCESS)
        return false;

    VkDescriptorImageInfo desc_img{};
    desc_img.sampler     = sampler_;
    desc_img.imageView   = out.view;
    desc_img.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkWriteDescriptorSet write{};
    write.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet          = out.desc_set;
    write.dstBinding      = 0;
    write.descriptorCount = 1;
    write.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo      = &desc_img;
    vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);

    return true;
}

void TextDemoRenderer::destroy_quad_textures() {
    for (auto& tex : textures_) {
        if (tex.view)   vkDestroyImageView(device_, tex.view, nullptr);
        if (tex.image)  vkDestroyImage(device_, tex.image, nullptr);
        if (tex.memory) vkFreeMemory(device_, tex.memory, nullptr);
    }
    textures_.clear();
    draw_entries_.clear();

    if (vertex_buffer_) {
        vkDestroyBuffer(device_, vertex_buffer_, nullptr);
        vkFreeMemory(device_, vertex_memory_, nullptr);
        vertex_buffer_ = VK_NULL_HANDLE;
        vertex_memory_ = VK_NULL_HANDLE;
    }

    // Reset descriptor pool (frees all allocated sets)
    if (desc_pool_) {
        vkResetDescriptorPool(device_, desc_pool_, 0);
    }
}

bool TextDemoRenderer::initialize(VulkanContext& vk_ctx, const char* shader_dir) {
    // Clean up any previous partial initialization
    if (device_) shutdown_partial();

    vk_ctx_ = &vk_ctx;
    device_ = vk_ctx.device();

    if (!create_descriptor_layout()) { shutdown_partial(); return false; }
    if (!create_sampler()) { shutdown_partial(); return false; }
    if (!create_pipeline(shader_dir)) { shutdown_partial(); return false; }

    // Descriptor pool — enough for many textures
    VkDescriptorPoolSize pool_size{};
    pool_size.type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    pool_size.descriptorCount = 64;

    VkDescriptorPoolCreateInfo pool_info{};
    pool_info.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_info.flags         = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    pool_info.maxSets       = 64;
    pool_info.poolSizeCount = 1;
    pool_info.pPoolSizes    = &pool_size;

    if (vkCreateDescriptorPool(device_, &pool_info, nullptr, &desc_pool_) != VK_SUCCESS) {
        shutdown_partial();
        return false;
    }

    initialized_ = true;
    return true;
}

void TextDemoRenderer::shutdown() {
    if (!initialized_) return;
    vkDeviceWaitIdle(device_);

    destroy_quad_textures();

    if (sampler_)         vkDestroySampler(device_, sampler_, nullptr);
    if (pipeline_)        vkDestroyPipeline(device_, pipeline_, nullptr);
    if (pipeline_layout_) vkDestroyPipelineLayout(device_, pipeline_layout_, nullptr);
    if (desc_set_layout_) vkDestroyDescriptorSetLayout(device_, desc_set_layout_, nullptr);
    if (desc_pool_)       vkDestroyDescriptorPool(device_, desc_pool_, nullptr);

    initialized_ = false;
}

void TextDemoRenderer::set_quads(const std::vector<TexturedQuad>& quads) {
    vkDeviceWaitIdle(device_);
    destroy_quad_textures();

    if (quads.empty()) return;

    float scr_w = static_cast<float>(vk_ctx_->swapchain_extent().width);
    float scr_h = static_cast<float>(vk_ctx_->swapchain_extent().height);

    // Generate vertices for all quads
    std::vector<QuadVertex> all_verts;
    draw_entries_.clear();

    for (const auto& q : quads) {
        // Convert pixel coords to NDC
        float x0 = (q.x / scr_w) * 2.0f - 1.0f;
        float y0 = (q.y / scr_h) * 2.0f - 1.0f;
        float x1 = ((q.x + q.w) / scr_w) * 2.0f - 1.0f;
        float y1 = ((q.y + q.h) / scr_h) * 2.0f - 1.0f;

        uint32_t first = static_cast<uint32_t>(all_verts.size());
        float c[] = {1,1,1,1}; // white — texture provides color

        all_verts.push_back({{x0, y0}, {0, 0}, {c[0],c[1],c[2],c[3]}});
        all_verts.push_back({{x1, y0}, {1, 0}, {c[0],c[1],c[2],c[3]}});
        all_verts.push_back({{x0, y1}, {0, 1}, {c[0],c[1],c[2],c[3]}});
        all_verts.push_back({{x1, y0}, {1, 0}, {c[0],c[1],c[2],c[3]}});
        all_verts.push_back({{x1, y1}, {1, 1}, {c[0],c[1],c[2],c[3]}});
        all_verts.push_back({{x0, y1}, {0, 1}, {c[0],c[1],c[2],c[3]}});

        // Upload texture
        QuadTexture tex;
        if (upload_texture(q.image, tex)) {
            textures_.push_back(tex);
            draw_entries_.push_back({first, 6, tex.desc_set});
        }
    }

    vertex_count_ = static_cast<uint32_t>(all_verts.size());
    if (vertex_count_ == 0) return;

    // Upload vertex buffer
    VkDeviceSize vb_size = vertex_count_ * sizeof(QuadVertex);
    create_buffer(vb_size, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                  vertex_buffer_, vertex_memory_);

    void* mapped;
    vkMapMemory(device_, vertex_memory_, 0, vb_size, 0, &mapped);
    memcpy(mapped, all_verts.data(), vb_size);
    vkUnmapMemory(device_, vertex_memory_);
}

void TextDemoRenderer::render(VkCommandBuffer cmd, VkRenderPass render_pass,
                               VkFramebuffer framebuffer, VkExtent2D extent,
                               const TextPushConstants& pc) {
    VkClearValue clear = {{{0.12f, 0.12f, 0.15f, 1.0f}}};

    VkRenderPassBeginInfo rp_info{};
    rp_info.sType       = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rp_info.renderPass  = render_pass;
    rp_info.framebuffer = framebuffer;
    rp_info.renderArea  = {{0, 0}, extent};
    rp_info.clearValueCount = 1;
    rp_info.pClearValues    = &clear;

    vkCmdBeginRenderPass(cmd, &rp_info, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);

    VkViewport viewport{0, 0, (float)extent.width, (float)extent.height, 0, 1};
    VkRect2D scissor{{0, 0}, extent};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    vkCmdPushConstants(cmd, pipeline_layout_, VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(TextPushConstants), &pc);

    if (vertex_buffer_) {
        VkDeviceSize offset = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &vertex_buffer_, &offset);

        for (const auto& entry : draw_entries_) {
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    pipeline_layout_, 0, 1, &entry.desc_set, 0, nullptr);
            vkCmdDraw(cmd, entry.vertex_count, 1, entry.first_vertex, 0);
        }
    }

    vkCmdEndRenderPass(cmd);
}

#endif // PICTOR_HAS_VULKAN

// ============================================================
// Demo content builders
// ============================================================

/// Build SVG demo quads: render "あ" and "薔薇" at multiple sizes
static std::vector<TexturedQuad> build_svg_demo(FontLoader& font_loader,
                                                 TextSvgRenderer& svg_renderer,
                                                 TextImageRenderer& img_renderer,
                                                 FontHandle font,
                                                 float screen_w, float screen_h) {
    std::vector<TexturedQuad> quads;

    // Title label
    {
        TextStyle title_style;
        title_style.font_size = 28.0f;
        title_style.color = {0.9f, 0.9f, 0.3f, 1.0f};

        ImageBuffer title_img = img_renderer.render_text(
            font, "SVG Vector Text - Resolution Independence", title_style);
        if (!title_img.pixels.empty()) {
            TexturedQuad q;
            q.x = 20.0f;
            q.y = 15.0f;
            q.w = static_cast<float>(title_img.width);
            q.h = static_cast<float>(title_img.height);
            q.image = std::move(title_img);
            quads.push_back(std::move(q));
        }
    }

    // Render "あ" at different sizes: 16, 32, 64, 128, 256
    const float sizes[] = {16.0f, 32.0f, 64.0f, 128.0f, 256.0f};
    float cursor_x = 30.0f;
    float cursor_y = 70.0f;

    // Section label for "あ"
    {
        TextStyle label_style;
        label_style.font_size = 20.0f;
        label_style.color = {0.6f, 0.8f, 1.0f, 1.0f};

        ImageBuffer label = img_renderer.render_text(font,
            "Glyph 'A' (U+3042) at 16, 32, 64, 128, 256px:", label_style);
        if (!label.pixels.empty()) {
            TexturedQuad q;
            q.x = cursor_x;
            q.y = cursor_y;
            q.w = static_cast<float>(label.width);
            q.h = static_cast<float>(label.height);
            q.image = std::move(label);
            quads.push_back(std::move(q));
        }
        cursor_y += 35.0f;
    }

    // Render "あ" using SVG outline → re-rasterize at each size
    for (float sz : sizes) {
        TextStyle style;
        style.font_size = sz;
        style.color = {1.0f, 1.0f, 1.0f, 1.0f};

        ImageBuffer glyph = img_renderer.render_text(font, "\xe3\x81\x82", style); // "あ"
        if (glyph.pixels.empty()) continue;

        TexturedQuad q;
        q.x = cursor_x;
        q.y = cursor_y;
        q.w = static_cast<float>(glyph.width);
        q.h = static_cast<float>(glyph.height);
        q.image = std::move(glyph);
        quads.push_back(std::move(q));

        cursor_x += q.w + 20.0f;
    }

    // Render "薔薇" at different sizes
    cursor_x = 30.0f;
    cursor_y += 280.0f;

    // Section label for "薔薇"
    {
        TextStyle label_style;
        label_style.font_size = 20.0f;
        label_style.color = {0.6f, 0.8f, 1.0f, 1.0f};

        ImageBuffer label = img_renderer.render_text(font,
            "Kanji compound (U+8594 U+8599) at 24, 48, 96, 192px:", label_style);
        if (!label.pixels.empty()) {
            TexturedQuad q;
            q.x = cursor_x;
            q.y = cursor_y;
            q.w = static_cast<float>(label.width);
            q.h = static_cast<float>(label.height);
            q.image = std::move(label);
            quads.push_back(std::move(q));
        }
        cursor_y += 35.0f;
    }

    const float sizes2[] = {24.0f, 48.0f, 96.0f, 192.0f};
    for (float sz : sizes2) {
        TextStyle style;
        style.font_size = sz;
        style.color = {1.0f, 0.95f, 0.9f, 1.0f};

        // "薔薇" in UTF-8
        ImageBuffer text = img_renderer.render_text(font,
            "\xe8\x96\x94\xe8\x96\x87", style);
        if (text.pixels.empty()) continue;

        TexturedQuad q;
        q.x = cursor_x;
        q.y = cursor_y;
        q.w = static_cast<float>(text.width);
        q.h = static_cast<float>(text.height);
        q.image = std::move(text);
        quads.push_back(std::move(q));

        cursor_x += q.w + 25.0f;
    }

    // SVG path visualization — show raw outline data
    cursor_x = 30.0f;
    cursor_y += 220.0f;

    {
        TextStyle label_style;
        label_style.font_size = 18.0f;
        label_style.color = {0.5f, 1.0f, 0.5f, 1.0f};

        // Extract outline info
        auto outline_a = svg_renderer.extract_glyph_outline(font, 0x3042); // あ
        auto outline_b = svg_renderer.extract_glyph_outline(font, 0x8594); // 薔

        char info_buf[256];
        std::snprintf(info_buf, sizeof(info_buf),
            "SVG outline: 'A' has %zu path commands, scale-independent rendering",
            outline_a.path.size());

        ImageBuffer info_img = img_renderer.render_text(font, info_buf, label_style);
        if (!info_img.pixels.empty()) {
            TexturedQuad q;
            q.x = cursor_x;
            q.y = cursor_y;
            q.w = static_cast<float>(info_img.width);
            q.h = static_cast<float>(info_img.height);
            q.image = std::move(info_img);
            quads.push_back(std::move(q));
        }
    }

    return quads;
}

/// Build rasterized text demo with effects
static std::vector<TexturedQuad> build_rasterized_demo(FontLoader& font_loader,
                                                        TextImageRenderer& img_renderer,
                                                        FontHandle font,
                                                        float screen_w, float screen_h) {
    std::vector<TexturedQuad> quads;

    // Title
    {
        TextStyle title_style;
        title_style.font_size = 28.0f;
        title_style.color = {0.9f, 0.9f, 0.3f, 1.0f};

        ImageBuffer title = img_renderer.render_text(
            font, "Rasterized Text + GPU Effects", title_style);
        if (!title.pixels.empty()) {
            TexturedQuad q;
            q.x = 20.0f;
            q.y = 15.0f;
            q.w = static_cast<float>(title.width);
            q.h = static_cast<float>(title.height);
            q.image = std::move(title);
            quads.push_back(std::move(q));
        }
    }

    float cursor_y = 70.0f;

    // Effect description
    {
        TextStyle desc_style;
        desc_style.font_size = 18.0f;
        desc_style.color = {0.6f, 0.8f, 1.0f, 1.0f};

        ImageBuffer desc = img_renderer.render_text(font,
            "Press 1-4 to cycle effects: None / Outline / Shadow / Glow",
            desc_style);
        if (!desc.pixels.empty()) {
            TexturedQuad q;
            q.x = 30.0f;
            q.y = cursor_y;
            q.w = static_cast<float>(desc.width);
            q.h = static_cast<float>(desc.height);
            q.image = std::move(desc);
            quads.push_back(std::move(q));
        }
        cursor_y += 45.0f;
    }

    // Large text — main showcase
    {
        TextStyle big_style;
        big_style.font_size = 72.0f;
        big_style.color = {1.0f, 1.0f, 1.0f, 1.0f};

        ImageBuffer big_text = img_renderer.render_text(font, "Pictor", big_style);
        if (!big_text.pixels.empty()) {
            TexturedQuad q;
            q.x = 40.0f;
            q.y = cursor_y;
            q.w = static_cast<float>(big_text.width);
            q.h = static_cast<float>(big_text.height);
            q.image = std::move(big_text);
            quads.push_back(std::move(q));
        }
        cursor_y += 100.0f;
    }

    // Japanese text with effects
    {
        TextStyle jp_style;
        jp_style.font_size = 56.0f;
        jp_style.color = {1.0f, 0.85f, 0.6f, 1.0f};

        // "テキスト描画" (Text Rendering)
        ImageBuffer jp_text = img_renderer.render_text(font,
            "\xe3\x83\x86\xe3\x82\xad\xe3\x82\xb9\xe3\x83\x88\xe6\x8f\x8f\xe7\x94\xbb",
            jp_style);
        if (!jp_text.pixels.empty()) {
            TexturedQuad q;
            q.x = 40.0f;
            q.y = cursor_y;
            q.w = static_cast<float>(jp_text.width);
            q.h = static_cast<float>(jp_text.height);
            q.image = std::move(jp_text);
            quads.push_back(std::move(q));
        }
        cursor_y += 85.0f;
    }

    // Multi-line mixed text
    {
        TextStyle mixed_style;
        mixed_style.font_size = 36.0f;
        mixed_style.color = {0.8f, 1.0f, 0.8f, 1.0f};

        ImageBuffer mixed = img_renderer.render_text(font,
            "Free-format text rendering", mixed_style);
        if (!mixed.pixels.empty()) {
            TexturedQuad q;
            q.x = 40.0f;
            q.y = cursor_y;
            q.w = static_cast<float>(mixed.width);
            q.h = static_cast<float>(mixed.height);
            q.image = std::move(mixed);
            quads.push_back(std::move(q));
        }
        cursor_y += 55.0f;
    }

    // Another line — showing different sizes
    {
        TextStyle small_style;
        small_style.font_size = 24.0f;
        small_style.color = {0.7f, 0.7f, 1.0f, 1.0f};

        ImageBuffer small_text = img_renderer.render_text(font,
            "with real-time GPU outline, shadow, and glow effects", small_style);
        if (!small_text.pixels.empty()) {
            TexturedQuad q;
            q.x = 50.0f;
            q.y = cursor_y;
            q.w = static_cast<float>(small_text.width);
            q.h = static_cast<float>(small_text.height);
            q.image = std::move(small_text);
            quads.push_back(std::move(q));
        }
        cursor_y += 50.0f;
    }

    // Colorful text samples
    struct ColoredText {
        const char* text;
        float font_size;
        float color[4];
    };
    ColoredText samples[] = {
        {"ABCDEFGHIJKLMNOPQRSTUVWXYZ", 32.0f, {1.0f, 0.4f, 0.4f, 1.0f}},
        {"abcdefghijklmnopqrstuvwxyz", 32.0f, {0.4f, 1.0f, 0.4f, 1.0f}},
        {"0123456789 !@#$%^&*()", 32.0f, {0.4f, 0.6f, 1.0f, 1.0f}},
    };

    for (const auto& sample : samples) {
        TextStyle s;
        s.font_size = sample.font_size;
        s.color = {sample.color[0], sample.color[1], sample.color[2], sample.color[3]};

        ImageBuffer img = img_renderer.render_text(font, sample.text, s);
        if (!img.pixels.empty()) {
            TexturedQuad q;
            q.x = 40.0f;
            q.y = cursor_y;
            q.w = static_cast<float>(img.width);
            q.h = static_cast<float>(img.height);
            q.image = std::move(img);
            quads.push_back(std::move(q));
        }
        cursor_y += 45.0f;
    }

    return quads;
}

// ============================================================
// Embedded minimal font (fallback if no system font found)
// ============================================================

/// Try to find a system font for the demo
static std::string find_default_font() {
    // Search for bundled default font (relative to exe: fonts/ and ../fonts/)
    const char* bundled[] = {
        "fonts/default.ttf",
        "fonts/default.otf",
        "../fonts/default.ttf",
        "../fonts/default.otf",
    };
    for (const char* path : bundled) {
        FILE* f = fopen(path, "rb");
        if (f) { fclose(f); return path; }
    }

    // Fallback: system fonts
    const char* system_fonts[] = {
        // Linux
        "/usr/share/fonts/truetype/noto/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/dejavu/DejaVuSans.ttf",
        // macOS
        "/System/Library/Fonts/Hiragino Sans GB.ttc",
        "/System/Library/Fonts/HelveticaNeue.ttc",
        // Windows
        "C:\\Windows\\Fonts\\msgothic.ttc",
        "C:\\Windows\\Fonts\\meiryo.ttc",
        "C:\\Windows\\Fonts\\arial.ttf",
    };
    for (const char* path : system_fonts) {
        FILE* f = fopen(path, "rb");
        if (f) { fclose(f); return path; }
    }
    return {};
}

// ============================================================
// Main
// ============================================================

int main() {
    printf("=== Pictor Text Rendering Demo ===\n\n");

    // ---- 1. GLFW Window ----
    GlfwSurfaceProvider surface_provider;
    GlfwWindowConfig win_cfg;
    win_cfg.width  = 1280;
    win_cfg.height = 720;
    win_cfg.title  = "Pictor — Text Rendering Demo";
    win_cfg.vsync  = true;

    if (!surface_provider.create(win_cfg)) {
        fprintf(stderr, "Failed to create GLFW window\n");
        return 1;
    }

    // ---- 2. Vulkan Context ----
    VulkanContext vk_ctx;
    VulkanContextConfig vk_cfg;
    vk_cfg.app_name   = "Pictor Text Demo";
    vk_cfg.validation = true;

    if (!vk_ctx.initialize(&surface_provider, vk_cfg)) {
        fprintf(stderr, "Failed to initialize Vulkan context\n");
        surface_provider.destroy();
        return 1;
    }

    printf("Vulkan initialized: %ux%u\n",
           vk_ctx.swapchain_extent().width,
           vk_ctx.swapchain_extent().height);

    // ---- 3. Pictor Renderer (pipeline metadata) ----
    RendererConfig pictor_cfg;
    pictor_cfg.initial_profile  = "Standard";
    pictor_cfg.screen_width     = vk_ctx.swapchain_extent().width;
    pictor_cfg.screen_height    = vk_ctx.swapchain_extent().height;
    pictor_cfg.profiler_enabled = false;

    PictorRenderer renderer;
    renderer.initialize(pictor_cfg);

    // ---- 4. Font loading ----
    FontLoader font_loader;
    std::string font_path = find_default_font();
    FontHandle font = INVALID_FONT;

    if (!font_path.empty()) {
        font = font_loader.load_from_file(font_path);
        if (font != INVALID_FONT) {
            printf("Loaded font: %s\n", font_path.c_str());
        }
    }

    if (font == INVALID_FONT) {
        fprintf(stderr, "No system font found. Text rendering requires a TTF/OTF font.\n");
        fprintf(stderr, "Install a font (e.g. Noto Sans CJK) and try again.\n");
        vk_ctx.shutdown();
        surface_provider.destroy();
        return 1;
    }

    TextSvgRenderer svg_renderer(font_loader);
    TextImageRenderer img_renderer(font_loader);

    // ---- 5. Text demo renderer ----
#ifdef PICTOR_HAS_VULKAN
    TextDemoRenderer text_renderer;
    if (!text_renderer.initialize(vk_ctx, "shaders")) {
        if (!text_renderer.initialize(vk_ctx, "../shaders")) {
            fprintf(stderr, "Failed to initialize text demo renderer\n");
            vk_ctx.shutdown();
            surface_provider.destroy();
            return 1;
        }
    }
#endif

    // ---- 6. Input callbacks ----
    GLFWwindow* win = surface_provider.glfw_window();

    glfwSetKeyCallback(win, [](GLFWwindow* w, int key, int /*scancode*/, int action, int /*mods*/) {
        if (action != GLFW_PRESS) return;

        if (key == GLFW_KEY_ESCAPE) {
            glfwSetWindowShouldClose(w, GLFW_TRUE);
        } else if (key == GLFW_KEY_LEFT) {
            g_demo_mode = (g_demo_mode + 1) % 2;
            g_needs_rebuild = true;
            printf("Mode: %s\n", mode_names[g_demo_mode]);
        } else if (key == GLFW_KEY_RIGHT) {
            g_demo_mode = (g_demo_mode + 1) % 2;
            g_needs_rebuild = true;
            printf("Mode: %s\n", mode_names[g_demo_mode]);
        } else if (key >= GLFW_KEY_1 && key <= GLFW_KEY_4) {
            g_effect = key - GLFW_KEY_1;
            printf("Effect: %s\n", effect_names[g_effect]);
        }
    });

    // ---- 7. Main loop ----
    float screen_w = static_cast<float>(vk_ctx.swapchain_extent().width);
    float screen_h = static_cast<float>(vk_ctx.swapchain_extent().height);

    printf("\nControls:\n");
    printf("  Left/Right : Switch demo mode\n");
    printf("  1-4        : Cycle effects (None/Outline/Shadow/Glow)\n");
    printf("  ESC        : Quit\n\n");

    Camera camera;
    camera.position = {0, 0, 1};
    for (int i = 0; i < 6; ++i) {
        camera.frustum.planes[i].normal   = {0, 0, 0};
        camera.frustum.planes[i].distance = 10000.0f;
    }

    uint64_t frame_count = 0;

    while (!surface_provider.should_close()) {
        surface_provider.poll_events();

        // Rebuild content if mode changed
        if (g_needs_rebuild) {
            g_needs_rebuild = false;

            std::vector<TexturedQuad> quads;
            if (g_demo_mode == 0) {
                quads = build_svg_demo(font_loader, svg_renderer, img_renderer,
                                       font, screen_w, screen_h);
            } else {
                quads = build_rasterized_demo(font_loader, img_renderer,
                                               font, screen_w, screen_h);
            }

#ifdef PICTOR_HAS_VULKAN
            text_renderer.set_quads(quads);
#endif
            printf("Built %zu text quads for mode '%s'\n",
                   quads.size(), mode_names[g_demo_mode]);
        }

        // Render
        uint32_t image_idx = vk_ctx.acquire_next_image();
        if (image_idx == UINT32_MAX) continue;

#ifdef PICTOR_HAS_VULKAN
        auto cmd = vk_ctx.command_buffers()[image_idx];
        vkResetCommandBuffer(cmd, 0);

        VkCommandBufferBeginInfo begin_info{};
        begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        vkBeginCommandBuffer(cmd, &begin_info);

        TextPushConstants pc{};
        pc.render_mode = 0; // textured
        pc.effect_mode = (g_demo_mode == 1) ? g_effect : 0;
        // Outline/shadow/glow color
        pc.outline_color[0] = 0.0f;
        pc.outline_color[1] = 0.0f;
        pc.outline_color[2] = 0.0f;
        pc.outline_color[3] = 1.0f;

        if (g_effect == 1) {
            // Outline: dark border
            pc.outline_color[0] = 0.1f;
            pc.outline_color[1] = 0.1f;
            pc.outline_color[2] = 0.1f;
            pc.outline_color[3] = 1.0f;
            pc.outline_width    = 2.0f;
        } else if (g_effect == 2) {
            // Drop shadow
            pc.outline_color[0] = 0.0f;
            pc.outline_color[1] = 0.0f;
            pc.outline_color[2] = 0.0f;
            pc.outline_color[3] = 0.7f;
            pc.outline_width    = 3.0f;
        } else if (g_effect == 3) {
            // Glow: bright color
            pc.outline_color[0] = 0.2f;
            pc.outline_color[1] = 0.5f;
            pc.outline_color[2] = 1.0f;
            pc.outline_color[3] = 1.0f;
            pc.outline_width    = 4.0f;
        }

        text_renderer.render(cmd, vk_ctx.default_render_pass(),
                             vk_ctx.framebuffers()[image_idx],
                             vk_ctx.swapchain_extent(), pc);

        vkEndCommandBuffer(cmd);

        // Submit
        VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSemaphore wait_sem  = vk_ctx.image_available_semaphore();
        VkSemaphore sig_sem   = vk_ctx.render_finished_semaphore();

        VkSubmitInfo submit_info{};
        submit_info.sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit_info.waitSemaphoreCount   = 1;
        submit_info.pWaitSemaphores      = &wait_sem;
        submit_info.pWaitDstStageMask    = &wait_stage;
        submit_info.commandBufferCount   = 1;
        submit_info.pCommandBuffers      = &cmd;
        submit_info.signalSemaphoreCount = 1;
        submit_info.pSignalSemaphores    = &sig_sem;

        vkQueueSubmit(vk_ctx.graphics_queue(), 1, &submit_info, vk_ctx.in_flight_fence());
#endif

        vk_ctx.present(image_idx);

        // Pictor pipeline tick
        renderer.begin_frame(1.0f / 60.0f);
        renderer.render(camera);
        renderer.end_frame();

        ++frame_count;
    }

    // ---- 8. Cleanup ----
    vk_ctx.device_wait_idle();

#ifdef PICTOR_HAS_VULKAN
    text_renderer.shutdown();
#endif

    font_loader.unload(font);
    renderer.shutdown();
    vk_ctx.shutdown();
    surface_provider.destroy();

    printf("\nText demo finished. %llu frames rendered.\n",
           static_cast<unsigned long long>(frame_count));
    return 0;
}
