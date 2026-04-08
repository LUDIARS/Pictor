#include "texture2d_renderer.h"

#ifdef PICTOR_HAS_VULKAN

#include "pictor/surface/vulkan_context.h"
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>
#include <string>

namespace pictor {

Texture2DRenderer::~Texture2DRenderer() {
    if (initialized_) shutdown();
}

bool Texture2DRenderer::initialize(VulkanContext& vk_ctx, const char* shader_dir) {
    printf("[Tex2D] Initializing...\n");
    vk_ctx_ = &vk_ctx;
    device_ = vk_ctx.device();

    if (!create_quad_buffer()) {
        fprintf(stderr, "[Tex2D] Failed to create quad buffer\n");
        return false;
    }
    printf("[Tex2D] Quad buffer created\n");

    if (!create_texture_resources()) {
        fprintf(stderr, "[Tex2D] Failed to create texture resources\n");
        return false;
    }
    printf("[Tex2D] Texture resources created (sampler + placeholder image)\n");

    if (!create_descriptor_sets()) {
        fprintf(stderr, "[Tex2D] Failed to create descriptor sets\n");
        return false;
    }
    printf("[Tex2D] Descriptor sets created\n");

    if (!create_pipeline(shader_dir)) {
        fprintf(stderr, "[Tex2D] Failed to create pipeline\n");
        return false;
    }
    printf("[Tex2D] Pipeline created\n");

    initialized_ = true;
    printf("[Tex2D] Initialization complete\n");
    return true;
}

void Texture2DRenderer::shutdown() {
    if (!initialized_) return;
    printf("[Tex2D] Shutting down...\n");
    vkDeviceWaitIdle(device_);

    if (pipeline_)        vkDestroyPipeline(device_, pipeline_, nullptr);
    if (pipeline_layout_) vkDestroyPipelineLayout(device_, pipeline_layout_, nullptr);
    if (desc_pool_)       vkDestroyDescriptorPool(device_, desc_pool_, nullptr);
    if (desc_set_layout_) vkDestroyDescriptorSetLayout(device_, desc_set_layout_, nullptr);

    if (sampler_)        vkDestroySampler(device_, sampler_, nullptr);
    if (texture_view_)   vkDestroyImageView(device_, texture_view_, nullptr);
    if (texture_image_)  vkDestroyImage(device_, texture_image_, nullptr);
    if (texture_memory_) vkFreeMemory(device_, texture_memory_, nullptr);

    auto destroy_buf = [&](VkBuffer& b, VkDeviceMemory& m) {
        if (b) vkDestroyBuffer(device_, b, nullptr);
        if (m) vkFreeMemory(device_, m, nullptr);
        b = VK_NULL_HANDLE; m = VK_NULL_HANDLE;
    };
    destroy_buf(vertex_buffer_, vertex_memory_);

    initialized_ = false;
    printf("[Tex2D] Shutdown complete\n");
}

// ─── Texture upload ──────────────────────────────────────────

bool Texture2DRenderer::upload_texture(const uint8_t* rgba_data, uint32_t width, uint32_t height) {
    printf("[Tex2D] Uploading texture %ux%u (%zu bytes)\n", width, height,
           static_cast<size_t>(width) * height * 4);

    // Destroy old texture if size changed
    if (texture_image_ && (tex_width_ != width || tex_height_ != height)) {
        printf("[Tex2D] Texture size changed, recreating image\n");
        vkDeviceWaitIdle(device_);
        if (texture_view_)   { vkDestroyImageView(device_, texture_view_, nullptr); texture_view_ = VK_NULL_HANDLE; }
        if (texture_image_)  { vkDestroyImage(device_, texture_image_, nullptr); texture_image_ = VK_NULL_HANDLE; }
        if (texture_memory_) { vkFreeMemory(device_, texture_memory_, nullptr); texture_memory_ = VK_NULL_HANDLE; }
    }

    tex_width_ = width;
    tex_height_ = height;
    VkDeviceSize image_size = static_cast<VkDeviceSize>(width) * height * 4;

    // Create staging buffer
    VkBuffer staging_buf = VK_NULL_HANDLE;
    VkDeviceMemory staging_mem = VK_NULL_HANDLE;
    if (!create_buffer(image_size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                       staging_buf, staging_mem)) {
        fprintf(stderr, "[Tex2D] Failed to create staging buffer\n");
        return false;
    }

    // Copy pixels to staging
    void* mapped = nullptr;
    vkMapMemory(device_, staging_mem, 0, image_size, 0, &mapped);
    memcpy(mapped, rgba_data, image_size);
    vkUnmapMemory(device_, staging_mem);
    printf("[Tex2D] Staging buffer uploaded\n");

    // Create image if needed
    if (!texture_image_) {
        VkImageCreateInfo img_info{};
        img_info.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        img_info.imageType     = VK_IMAGE_TYPE_2D;
        img_info.format        = VK_FORMAT_R8G8B8A8_UNORM;
        img_info.extent        = {width, height, 1};
        img_info.mipLevels     = 1;
        img_info.arrayLayers   = 1;
        img_info.samples       = VK_SAMPLE_COUNT_1_BIT;
        img_info.tiling        = VK_IMAGE_TILING_OPTIMAL;
        img_info.usage         = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        img_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        if (vkCreateImage(device_, &img_info, nullptr, &texture_image_) != VK_SUCCESS) {
            fprintf(stderr, "[Tex2D] Failed to create texture image\n");
            vkDestroyBuffer(device_, staging_buf, nullptr);
            vkFreeMemory(device_, staging_mem, nullptr);
            return false;
        }

        VkMemoryRequirements mem_req;
        vkGetImageMemoryRequirements(device_, texture_image_, &mem_req);

        VkMemoryAllocateInfo alloc{};
        alloc.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        alloc.allocationSize  = mem_req.size;
        alloc.memoryTypeIndex = find_memory_type(mem_req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

        if (vkAllocateMemory(device_, &alloc, nullptr, &texture_memory_) != VK_SUCCESS) {
            fprintf(stderr, "[Tex2D] Failed to allocate texture memory\n");
            vkDestroyBuffer(device_, staging_buf, nullptr);
            vkFreeMemory(device_, staging_mem, nullptr);
            return false;
        }
        vkBindImageMemory(device_, texture_image_, texture_memory_, 0);
        printf("[Tex2D] Image created and memory bound\n");
    }

    // Transition + copy via one-shot command buffer
    VkCommandBuffer cmd = vk_ctx_->begin_single_time_commands();
    if (!cmd) {
        // Fallback: use a temporary command buffer from the pool
        fprintf(stderr, "[Tex2D] begin_single_time_commands not available, using manual command buffer\n");
        vkDestroyBuffer(device_, staging_buf, nullptr);
        vkFreeMemory(device_, staging_mem, nullptr);
        return false;
    }

    // Transition UNDEFINED → TRANSFER_DST
    VkImageMemoryBarrier barrier{};
    barrier.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image               = texture_image_;
    barrier.subresourceRange    = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    barrier.srcAccessMask       = 0;
    barrier.dstAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;

    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &barrier);

    // Copy staging → image
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent      = {width, height, 1};
    vkCmdCopyBufferToImage(cmd, staging_buf, texture_image_,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    // Transition TRANSFER_DST → SHADER_READ_ONLY
    barrier.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &barrier);

    vk_ctx_->end_single_time_commands(cmd);
    printf("[Tex2D] Image layout transitioned and pixels copied\n");

    // Cleanup staging
    vkDestroyBuffer(device_, staging_buf, nullptr);
    vkFreeMemory(device_, staging_mem, nullptr);

    // Create image view if needed
    if (!texture_view_) {
        VkImageViewCreateInfo view_info{};
        view_info.sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view_info.image    = texture_image_;
        view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view_info.format   = VK_FORMAT_R8G8B8A8_UNORM;
        view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

        if (vkCreateImageView(device_, &view_info, nullptr, &texture_view_) != VK_SUCCESS) {
            fprintf(stderr, "[Tex2D] Failed to create image view\n");
            return false;
        }
        printf("[Tex2D] Image view created\n");
    }

    // Update descriptor set
    VkDescriptorImageInfo desc_img{};
    desc_img.sampler     = sampler_;
    desc_img.imageView   = texture_view_;
    desc_img.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkWriteDescriptorSet write{};
    write.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet          = desc_set_;
    write.dstBinding      = 0;
    write.descriptorCount = 1;
    write.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo      = &desc_img;

    vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
    printf("[Tex2D] Descriptor set updated\n");

    texture_uploaded_ = true;
    return true;
}

// ─── Render ──────────────────────────────────────────────────

void Texture2DRenderer::render(VkCommandBuffer cmd, VkExtent2D extent,
                                const Texture2DPushConstants& pc) {
    if (!initialized_ || !texture_uploaded_) {
        return;
    }

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            pipeline_layout_, 0, 1, &desc_set_, 0, nullptr);

    VkViewport viewport{};
    viewport.width    = static_cast<float>(extent.width);
    viewport.height   = static_cast<float>(extent.height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &viewport);

    VkRect2D scissor = {{0, 0}, extent};
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    vkCmdPushConstants(cmd, pipeline_layout_,
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(Texture2DPushConstants), &pc);

    VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &vertex_buffer_, &offset);
    vkCmdDraw(cmd, 6, 1, 0, 0);
}

// ─── Internals ───────────────────────────────────────────────

bool Texture2DRenderer::create_quad_buffer() {
    // Fullscreen quad: position (vec2) + uv (vec2)
    // UV is flipped vertically so that V=0 maps to the top of the screen
    // (image row 0 = top, Vulkan Y=-0.5 = top)
    Vertex2D vertices[] = {
        {{-0.5f, -0.5f}, {0.0f, 1.0f}},
        {{ 0.5f, -0.5f}, {1.0f, 1.0f}},
        {{ 0.5f,  0.5f}, {1.0f, 0.0f}},
        {{-0.5f, -0.5f}, {0.0f, 1.0f}},
        {{ 0.5f,  0.5f}, {1.0f, 0.0f}},
        {{-0.5f,  0.5f}, {0.0f, 0.0f}},
    };

    VkDeviceSize size = sizeof(vertices);
    if (!create_buffer(size,
                       VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                       vertex_buffer_, vertex_memory_))
        return false;

    void* mapped = nullptr;
    vkMapMemory(device_, vertex_memory_, 0, size, 0, &mapped);
    memcpy(mapped, vertices, size);
    vkUnmapMemory(device_, vertex_memory_);

    return true;
}

bool Texture2DRenderer::create_texture_resources() {
    // Create sampler
    VkSamplerCreateInfo sampler_info{};
    sampler_info.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler_info.magFilter    = VK_FILTER_LINEAR;
    sampler_info.minFilter    = VK_FILTER_LINEAR;
    sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_LINEAR;

    if (vkCreateSampler(device_, &sampler_info, nullptr, &sampler_) != VK_SUCCESS)
        return false;

    // Create a 1x1 white placeholder texture so the descriptor set is valid before upload
    uint8_t white_pixel[] = {255, 255, 255, 255};

    VkImageCreateInfo img_info{};
    img_info.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    img_info.imageType     = VK_IMAGE_TYPE_2D;
    img_info.format        = VK_FORMAT_R8G8B8A8_UNORM;
    img_info.extent        = {1, 1, 1};
    img_info.mipLevels     = 1;
    img_info.arrayLayers   = 1;
    img_info.samples       = VK_SAMPLE_COUNT_1_BIT;
    img_info.tiling        = VK_IMAGE_TILING_OPTIMAL;
    img_info.usage         = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    img_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    if (vkCreateImage(device_, &img_info, nullptr, &texture_image_) != VK_SUCCESS)
        return false;

    VkMemoryRequirements mem_req;
    vkGetImageMemoryRequirements(device_, texture_image_, &mem_req);

    VkMemoryAllocateInfo alloc{};
    alloc.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize  = mem_req.size;
    alloc.memoryTypeIndex = find_memory_type(mem_req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    if (vkAllocateMemory(device_, &alloc, nullptr, &texture_memory_) != VK_SUCCESS)
        return false;
    vkBindImageMemory(device_, texture_image_, texture_memory_, 0);

    tex_width_ = 1;
    tex_height_ = 1;

    // Create image view
    VkImageViewCreateInfo view_info{};
    view_info.sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.image    = texture_image_;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format   = VK_FORMAT_R8G8B8A8_UNORM;
    view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    if (vkCreateImageView(device_, &view_info, nullptr, &texture_view_) != VK_SUCCESS)
        return false;

    return true;
}

bool Texture2DRenderer::create_descriptor_sets() {
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

    // Pool
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

    // Allocate set
    VkDescriptorSetAllocateInfo alloc{};
    alloc.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc.descriptorPool     = desc_pool_;
    alloc.descriptorSetCount = 1;
    alloc.pSetLayouts        = &desc_set_layout_;

    if (vkAllocateDescriptorSets(device_, &alloc, &desc_set_) != VK_SUCCESS)
        return false;

    // Write placeholder texture to descriptor
    VkDescriptorImageInfo desc_img{};
    desc_img.sampler     = sampler_;
    desc_img.imageView   = texture_view_;
    desc_img.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkWriteDescriptorSet write{};
    write.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet          = desc_set_;
    write.dstBinding      = 0;
    write.descriptorCount = 1;
    write.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo      = &desc_img;

    vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);

    return true;
}

bool Texture2DRenderer::create_pipeline(const char* shader_dir) {
    std::string vert_path = std::string(shader_dir) + "/texture2d.vert.spv";
    std::string frag_path = std::string(shader_dir) + "/texture2d.frag.spv";

    printf("[Tex2D] Loading shaders: %s, %s\n", vert_path.c_str(), frag_path.c_str());

    VkShaderModule vert = load_shader(vert_path.c_str());
    VkShaderModule frag = load_shader(frag_path.c_str());
    if (!vert || !frag) {
        fprintf(stderr, "[Tex2D] Shader loading failed\n");
        if (vert) vkDestroyShaderModule(device_, vert, nullptr);
        if (frag) vkDestroyShaderModule(device_, frag, nullptr);
        return false;
    }

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vert;
    stages[0].pName  = "main";
    stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = frag;
    stages[1].pName  = "main";

    // Vertex input: pos(vec2) + uv(vec2)
    VkVertexInputBindingDescription bind{};
    bind.binding   = 0;
    bind.stride    = sizeof(Vertex2D);
    bind.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    VkVertexInputAttributeDescription attrs[2]{};
    attrs[0].location = 0;
    attrs[0].binding  = 0;
    attrs[0].format   = VK_FORMAT_R32G32_SFLOAT;
    attrs[0].offset   = offsetof(Vertex2D, pos);
    attrs[1].location = 1;
    attrs[1].binding  = 0;
    attrs[1].format   = VK_FORMAT_R32G32_SFLOAT;
    attrs[1].offset   = offsetof(Vertex2D, uv);

    VkPipelineVertexInputStateCreateInfo vertex_input{};
    vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertex_input.vertexBindingDescriptionCount   = 1;
    vertex_input.pVertexBindingDescriptions      = &bind;
    vertex_input.vertexAttributeDescriptionCount = 2;
    vertex_input.pVertexAttributeDescriptions    = attrs;

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
    raster.cullMode    = VK_CULL_MODE_NONE; // No culling for 2D quad
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
    blend_att.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
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

    // Push constant range
    VkPushConstantRange push_range{};
    push_range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    push_range.offset     = 0;
    push_range.size       = sizeof(Texture2DPushConstants);

    VkPipelineLayoutCreateInfo layout_info{};
    layout_info.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout_info.setLayoutCount         = 1;
    layout_info.pSetLayouts            = &desc_set_layout_;
    layout_info.pushConstantRangeCount = 1;
    layout_info.pPushConstantRanges    = &push_range;

    if (vkCreatePipelineLayout(device_, &layout_info, nullptr, &pipeline_layout_) != VK_SUCCESS) {
        fprintf(stderr, "[Tex2D] Failed to create pipeline layout\n");
        vkDestroyShaderModule(device_, vert, nullptr);
        vkDestroyShaderModule(device_, frag, nullptr);
        return false;
    }

    VkGraphicsPipelineCreateInfo pi{};
    pi.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pi.stageCount          = 2;
    pi.pStages             = stages;
    pi.pVertexInputState   = &vertex_input;
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

    if (result != VK_SUCCESS) {
        fprintf(stderr, "[Tex2D] Failed to create graphics pipeline (VkResult=%d)\n", result);
        return false;
    }

    printf("[Tex2D] Graphics pipeline created successfully\n");
    return true;
}

VkShaderModule Texture2DRenderer::load_shader(const char* path) {
    std::ifstream file(path, std::ios::ate | std::ios::binary);
    if (!file.is_open()) {
        fprintf(stderr, "[Tex2D] Cannot open shader: %s\n", path);
        return VK_NULL_HANDLE;
    }

    size_t size = static_cast<size_t>(file.tellg());
    std::vector<char> code(size);
    file.seekg(0);
    file.read(code.data(), static_cast<std::streamsize>(size));
    printf("[Tex2D] Loaded shader: %s (%zu bytes)\n", path, size);

    VkShaderModuleCreateInfo info{};
    info.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    info.codeSize = size;
    info.pCode    = reinterpret_cast<const uint32_t*>(code.data());

    VkShaderModule mod = VK_NULL_HANDLE;
    if (vkCreateShaderModule(device_, &info, nullptr, &mod) != VK_SUCCESS) {
        fprintf(stderr, "[Tex2D] Failed to create shader module: %s\n", path);
    }
    return mod;
}

uint32_t Texture2DRenderer::find_memory_type(uint32_t type_filter, VkMemoryPropertyFlags props) {
    VkPhysicalDeviceMemoryProperties mem_props;
    vkGetPhysicalDeviceMemoryProperties(vk_ctx_->physical_device(), &mem_props);

    for (uint32_t i = 0; i < mem_props.memoryTypeCount; ++i) {
        if ((type_filter & (1 << i)) &&
            (mem_props.memoryTypes[i].propertyFlags & props) == props) {
            return i;
        }
    }
    fprintf(stderr, "[Tex2D] Failed to find suitable memory type\n");
    return 0;
}

bool Texture2DRenderer::create_buffer(VkDeviceSize size, VkBufferUsageFlags usage,
                                       VkMemoryPropertyFlags props, VkBuffer& buf, VkDeviceMemory& mem) {
    VkBufferCreateInfo buf_info{};
    buf_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buf_info.size  = size;
    buf_info.usage = usage;

    if (vkCreateBuffer(device_, &buf_info, nullptr, &buf) != VK_SUCCESS)
        return false;

    VkMemoryRequirements mem_req;
    vkGetBufferMemoryRequirements(device_, buf, &mem_req);

    VkMemoryAllocateInfo alloc{};
    alloc.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize  = mem_req.size;
    alloc.memoryTypeIndex = find_memory_type(mem_req.memoryTypeBits, props);

    if (vkAllocateMemory(device_, &alloc, nullptr, &mem) != VK_SUCCESS) {
        vkDestroyBuffer(device_, buf, nullptr);
        buf = VK_NULL_HANDLE;
        return false;
    }

    vkBindBufferMemory(device_, buf, mem, 0);
    return true;
}

} // namespace pictor

#endif // PICTOR_HAS_VULKAN
