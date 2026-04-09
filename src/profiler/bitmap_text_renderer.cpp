#include "pictor/profiler/bitmap_text_renderer.h"

#ifdef PICTOR_HAS_VULKAN

#include "pictor/profiler/bitmap_font.h"
#include "pictor/surface/vulkan_context.h"
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>
#include <string>

namespace pictor {

BitmapTextRenderer::~BitmapTextRenderer() {
    if (initialized_) shutdown();
}

bool BitmapTextRenderer::initialize(VulkanContext& vk_ctx, const char* shader_dir) {
    vk_ctx_ = &vk_ctx;
    device_ = vk_ctx.device();

    if (!create_font_texture())       return false;
    if (!create_vertex_buffer())      return false;
    if (!create_descriptor_sets())    return false;
    if (!create_pipeline(shader_dir)) return false;

    initialized_ = true;
    printf("[BitmapTextRenderer] Initialized\n");
    return true;
}

void BitmapTextRenderer::shutdown() {
    if (!initialized_) return;
    vkDeviceWaitIdle(device_);

    if (pipeline_)        vkDestroyPipeline(device_, pipeline_, nullptr);
    if (pipeline_layout_) vkDestroyPipelineLayout(device_, pipeline_layout_, nullptr);
    if (desc_pool_)       vkDestroyDescriptorPool(device_, desc_pool_, nullptr);
    if (desc_set_layout_) vkDestroyDescriptorSetLayout(device_, desc_set_layout_, nullptr);

    if (font_sampler_)      vkDestroySampler(device_, font_sampler_, nullptr);
    if (font_view_)         vkDestroyImageView(device_, font_view_, nullptr);
    if (font_image_)        vkDestroyImage(device_, font_image_, nullptr);
    if (font_image_memory_) vkFreeMemory(device_, font_image_memory_, nullptr);

    if (vertex_buffer_) vkDestroyBuffer(device_, vertex_buffer_, nullptr);
    if (vertex_memory_) vkFreeMemory(device_, vertex_memory_, nullptr);

    initialized_ = false;
}

// ---- Per-frame API ----

void BitmapTextRenderer::begin(VkCommandBuffer cmd, VkExtent2D extent) {
    current_cmd_ = cmd;
    current_extent_ = extent;
    vertices_.clear();
}

void BitmapTextRenderer::draw_text(float x, float y, const char* text,
                                    float r, float g, float b, float a) {
    if (!text) return;

    float gw = static_cast<float>(GLYPH_W) * scale_;
    float gh = static_cast<float>(GLYPH_H) * scale_;
    float cursor_x = x;

    for (const char* p = text; *p; ++p) {
        char c = *p;
        if (c == '\n') {
            cursor_x = x;
            y += gh;
            continue;
        }

        if (vertices_.size() + 6 > MAX_VERTICES) break;

        float u0, v0, u1, v1;
        glyph_uv(c, u0, v0, u1, v1);

        float x0 = cursor_x;
        float y0 = y;
        float x1 = cursor_x + gw;
        float y1 = y + gh;

        // Two triangles per quad
        vertices_.push_back({{x0, y0}, {u0, v0}, {r, g, b, a}});
        vertices_.push_back({{x1, y0}, {u1, v0}, {r, g, b, a}});
        vertices_.push_back({{x0, y1}, {u0, v1}, {r, g, b, a}});
        vertices_.push_back({{x1, y0}, {u1, v0}, {r, g, b, a}});
        vertices_.push_back({{x1, y1}, {u1, v1}, {r, g, b, a}});
        vertices_.push_back({{x0, y1}, {u0, v1}, {r, g, b, a}});

        cursor_x += gw;
    }
}

void BitmapTextRenderer::end() {
    if (!initialized_ || vertices_.empty() || !current_cmd_) return;

    // Upload vertices
    VkDeviceSize size = vertices_.size() * sizeof(TextVertex);
    void* mapped = nullptr;
    vkMapMemory(device_, vertex_memory_, 0, size, 0, &mapped);
    memcpy(mapped, vertices_.data(), size);
    vkUnmapMemory(device_, vertex_memory_);

    // Record draw commands (render pass is already active)
    vkCmdBindPipeline(current_cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);

    // Push constants: screen dimensions
    float push_data[2] = {
        static_cast<float>(current_extent_.width),
        static_cast<float>(current_extent_.height)
    };
    vkCmdPushConstants(current_cmd_, pipeline_layout_,
                       VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(push_data), push_data);

    vkCmdBindDescriptorSets(current_cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            pipeline_layout_, 0, 1, &desc_set_, 0, nullptr);

    VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(current_cmd_, 0, 1, &vertex_buffer_, &offset);

    VkViewport viewport{};
    viewport.width  = static_cast<float>(current_extent_.width);
    viewport.height = static_cast<float>(current_extent_.height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(current_cmd_, 0, 1, &viewport);

    VkRect2D scissor = {{0, 0}, current_extent_};
    vkCmdSetScissor(current_cmd_, 0, 1, &scissor);

    vkCmdDraw(current_cmd_, static_cast<uint32_t>(vertices_.size()), 1, 0, 0);

    current_cmd_ = VK_NULL_HANDLE;
}

// ---- Font texture ----

bool BitmapTextRenderer::create_font_texture() {
    // Unpack font atlas
    std::vector<uint8_t> pixels(ATLAS_W * ATLAS_H);
    unpack_font_atlas(pixels.data());

    // Create staging buffer
    VkDeviceSize image_size = ATLAS_W * ATLAS_H;
    VkBuffer staging_buf = VK_NULL_HANDLE;
    VkDeviceMemory staging_mem = VK_NULL_HANDLE;

    if (!create_buffer(image_size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                       staging_buf, staging_mem))
        return false;

    void* mapped = nullptr;
    vkMapMemory(device_, staging_mem, 0, image_size, 0, &mapped);
    memcpy(mapped, pixels.data(), image_size);
    vkUnmapMemory(device_, staging_mem);

    // Create image
    VkImageCreateInfo img_info{};
    img_info.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    img_info.imageType     = VK_IMAGE_TYPE_2D;
    img_info.format        = VK_FORMAT_R8_UNORM;
    img_info.extent        = {ATLAS_W, ATLAS_H, 1};
    img_info.mipLevels     = 1;
    img_info.arrayLayers   = 1;
    img_info.samples       = VK_SAMPLE_COUNT_1_BIT;
    img_info.tiling        = VK_IMAGE_TILING_OPTIMAL;
    img_info.usage         = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    img_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    if (vkCreateImage(device_, &img_info, nullptr, &font_image_) != VK_SUCCESS) return false;

    VkMemoryRequirements mem_req;
    vkGetImageMemoryRequirements(device_, font_image_, &mem_req);

    VkMemoryAllocateInfo alloc{};
    alloc.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize  = mem_req.size;
    alloc.memoryTypeIndex = find_memory_type(mem_req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    if (vkAllocateMemory(device_, &alloc, nullptr, &font_image_memory_) != VK_SUCCESS) return false;
    vkBindImageMemory(device_, font_image_, font_image_memory_, 0);

    // One-shot command buffer for upload
    VkCommandBufferAllocateInfo cmd_alloc{};
    cmd_alloc.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cmd_alloc.commandPool        = vk_ctx_->command_pool();
    cmd_alloc.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmd_alloc.commandBufferCount = 1;

    VkCommandBuffer cmd;
    vkAllocateCommandBuffers(device_, &cmd_alloc, &cmd);

    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &begin);

    // Transition UNDEFINED -> TRANSFER_DST
    VkImageMemoryBarrier barrier{};
    barrier.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image               = font_image_;
    barrier.subresourceRange    = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    barrier.srcAccessMask       = 0;
    barrier.dstAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;

    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &barrier);

    // Copy buffer to image
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent      = {ATLAS_W, ATLAS_H, 1};

    vkCmdCopyBufferToImage(cmd, staging_buf, font_image_,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    // Transition TRANSFER_DST -> SHADER_READ_ONLY
    barrier.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &barrier);

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
    view_info.sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.image    = font_image_;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format   = VK_FORMAT_R8_UNORM;
    view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    if (vkCreateImageView(device_, &view_info, nullptr, &font_view_) != VK_SUCCESS) return false;

    // Sampler (nearest for pixel-perfect text)
    VkSamplerCreateInfo samp_info{};
    samp_info.sType     = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samp_info.magFilter = VK_FILTER_NEAREST;
    samp_info.minFilter = VK_FILTER_NEAREST;
    samp_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samp_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samp_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;

    if (vkCreateSampler(device_, &samp_info, nullptr, &font_sampler_) != VK_SUCCESS) return false;

    return true;
}

// ---- Pipeline ----

VkShaderModule BitmapTextRenderer::load_shader(const char* path) {
    std::ifstream file(path, std::ios::ate | std::ios::binary);
    if (!file.is_open()) {
        fprintf(stderr, "[BitmapTextRenderer] Cannot open shader: %s\n", path);
        return VK_NULL_HANDLE;
    }

    size_t size = static_cast<size_t>(file.tellg());
    std::vector<char> code(size);
    file.seekg(0);
    file.read(code.data(), static_cast<std::streamsize>(size));

    VkShaderModuleCreateInfo info{};
    info.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    info.codeSize = size;
    info.pCode    = reinterpret_cast<const uint32_t*>(code.data());

    VkShaderModule mod = VK_NULL_HANDLE;
    vkCreateShaderModule(device_, &info, nullptr, &mod);
    return mod;
}

bool BitmapTextRenderer::create_pipeline(const char* shader_dir) {
    std::string vert_path = std::string(shader_dir) + "/text_overlay.vert.spv";
    std::string frag_path = std::string(shader_dir) + "/text_overlay.frag.spv";

    VkShaderModule vert = load_shader(vert_path.c_str());
    VkShaderModule frag = load_shader(frag_path.c_str());
    if (!vert || !frag) return false;

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vert;
    stages[0].pName  = "main";
    stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = frag;
    stages[1].pName  = "main";

    // Vertex input: pos(vec2) + uv(vec2) + color(vec4)
    VkVertexInputBindingDescription bind{};
    bind.binding   = 0;
    bind.stride    = sizeof(TextVertex);
    bind.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    VkVertexInputAttributeDescription attrs[3]{};
    attrs[0] = {0, 0, VK_FORMAT_R32G32_SFLOAT,       offsetof(TextVertex, pos)};
    attrs[1] = {1, 0, VK_FORMAT_R32G32_SFLOAT,       offsetof(TextVertex, uv)};
    attrs[2] = {2, 0, VK_FORMAT_R32G32B32A32_SFLOAT,  offsetof(TextVertex, color)};

    VkPipelineVertexInputStateCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vi.vertexBindingDescriptionCount   = 1;
    vi.pVertexBindingDescriptions      = &bind;
    vi.vertexAttributeDescriptionCount = 3;
    vi.pVertexAttributeDescriptions    = attrs;

    VkPipelineInputAssemblyStateCreateInfo ia{};
    ia.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo vp{};
    vp.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.viewportCount = 1;
    vp.scissorCount  = 1;

    VkPipelineRasterizationStateCreateInfo raster{};
    raster.sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode    = VK_CULL_MODE_NONE;
    raster.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth   = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo ds{};
    ds.sType            = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds.depthTestEnable  = VK_FALSE;
    ds.depthWriteEnable = VK_FALSE;

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

    VkDynamicState dyn_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dyn{};
    dyn.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dyn.dynamicStateCount = 2;
    dyn.pDynamicStates    = dyn_states;

    // Push constant for screen dimensions
    VkPushConstantRange push_range{};
    push_range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    push_range.offset     = 0;
    push_range.size       = 2 * sizeof(float);

    VkPipelineLayoutCreateInfo layout_info{};
    layout_info.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout_info.setLayoutCount         = 1;
    layout_info.pSetLayouts            = &desc_set_layout_;
    layout_info.pushConstantRangeCount = 1;
    layout_info.pPushConstantRanges    = &push_range;

    if (vkCreatePipelineLayout(device_, &layout_info, nullptr, &pipeline_layout_) != VK_SUCCESS) {
        vkDestroyShaderModule(device_, vert, nullptr);
        vkDestroyShaderModule(device_, frag, nullptr);
        return false;
    }

    VkGraphicsPipelineCreateInfo pi{};
    pi.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pi.stageCount          = 2;
    pi.pStages             = stages;
    pi.pVertexInputState   = &vi;
    pi.pInputAssemblyState = &ia;
    pi.pViewportState      = &vp;
    pi.pRasterizationState = &raster;
    pi.pMultisampleState   = &ms;
    pi.pDepthStencilState  = &ds;
    pi.pColorBlendState    = &blend;
    pi.pDynamicState       = &dyn;
    pi.layout              = pipeline_layout_;
    pi.renderPass          = vk_ctx_->default_render_pass();
    pi.subpass             = 0;

    VkResult result = vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &pi, nullptr, &pipeline_);

    vkDestroyShaderModule(device_, vert, nullptr);
    vkDestroyShaderModule(device_, frag, nullptr);

    return result == VK_SUCCESS;
}

// ---- Buffer helpers ----

uint32_t BitmapTextRenderer::find_memory_type(uint32_t type_filter, VkMemoryPropertyFlags props) {
    VkPhysicalDeviceMemoryProperties mem_props;
    vkGetPhysicalDeviceMemoryProperties(vk_ctx_->physical_device(), &mem_props);
    for (uint32_t i = 0; i < mem_props.memoryTypeCount; ++i) {
        if ((type_filter & (1 << i)) &&
            (mem_props.memoryTypes[i].propertyFlags & props) == props)
            return i;
    }
    return UINT32_MAX;
}

bool BitmapTextRenderer::create_buffer(VkDeviceSize size, VkBufferUsageFlags usage,
                                        VkMemoryPropertyFlags props,
                                        VkBuffer& buf, VkDeviceMemory& mem) {
    VkBufferCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.size  = size;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateBuffer(device_, &info, nullptr, &buf) != VK_SUCCESS) return false;

    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(device_, buf, &req);

    VkMemoryAllocateInfo alloc{};
    alloc.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize  = req.size;
    alloc.memoryTypeIndex = find_memory_type(req.memoryTypeBits, props);
    if (alloc.memoryTypeIndex == UINT32_MAX) return false;

    if (vkAllocateMemory(device_, &alloc, nullptr, &mem) != VK_SUCCESS) return false;
    vkBindBufferMemory(device_, buf, mem, 0);
    return true;
}

bool BitmapTextRenderer::create_vertex_buffer() {
    VkDeviceSize size = MAX_VERTICES * sizeof(TextVertex);
    return create_buffer(size, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                         vertex_buffer_, vertex_memory_);
}

bool BitmapTextRenderer::create_descriptor_sets() {
    // Layout: binding 0 = combined image sampler
    VkDescriptorSetLayoutBinding binding{};
    binding.binding         = 0;
    binding.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = 1;
    binding.stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo layout_info{};
    layout_info.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layout_info.bindingCount = 1;
    layout_info.pBindings    = &binding;

    if (vkCreateDescriptorSetLayout(device_, &layout_info, nullptr, &desc_set_layout_) != VK_SUCCESS)
        return false;

    VkDescriptorPoolSize pool_size{};
    pool_size.type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    pool_size.descriptorCount = 1;

    VkDescriptorPoolCreateInfo pool_info{};
    pool_info.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_info.maxSets       = 1;
    pool_info.poolSizeCount = 1;
    pool_info.pPoolSizes    = &pool_size;

    if (vkCreateDescriptorPool(device_, &pool_info, nullptr, &desc_pool_) != VK_SUCCESS)
        return false;

    VkDescriptorSetAllocateInfo alloc{};
    alloc.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc.descriptorPool     = desc_pool_;
    alloc.descriptorSetCount = 1;
    alloc.pSetLayouts        = &desc_set_layout_;

    if (vkAllocateDescriptorSets(device_, &alloc, &desc_set_) != VK_SUCCESS)
        return false;

    VkDescriptorImageInfo img_info{};
    img_info.sampler     = font_sampler_;
    img_info.imageView   = font_view_;
    img_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkWriteDescriptorSet write{};
    write.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet          = desc_set_;
    write.dstBinding      = 0;
    write.descriptorCount = 1;
    write.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo      = &img_info;

    vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
    return true;
}

} // namespace pictor

#endif // PICTOR_HAS_VULKAN
