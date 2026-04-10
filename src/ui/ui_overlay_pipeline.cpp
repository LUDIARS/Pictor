#include "pictor/ui/ui_overlay_pipeline.h"

#ifdef PICTOR_HAS_VULKAN

#include "pictor/surface/vulkan_context.h"
#include <cstdio>
#include <cstring>
#include <fstream>
#include <algorithm>

namespace pictor {

UIOverlayPipeline::~UIOverlayPipeline() {
    if (initialized_) shutdown();
}

// ─── Lifecycle ───────────────────────────────────────────────

bool UIOverlayPipeline::initialize(VulkanContext& vk_ctx, const char* shader_dir,
                                    const UIOverlayConfig& config) {
    printf("[UIOverlay] Initializing (max_vertices=%u, max_textures=%u)...\n",
           config.max_vertices, config.max_textures);
    vk_ctx_ = &vk_ctx;
    device_ = vk_ctx.device();
    config_ = config;

    if (!create_sampler()) {
        fprintf(stderr, "[UIOverlay] Failed to create sampler\n");
        return false;
    }
    printf("[UIOverlay] Sampler created\n");

    if (!create_descriptor_pool()) {
        fprintf(stderr, "[UIOverlay] Failed to create descriptor pool\n");
        return false;
    }
    printf("[UIOverlay] Descriptor pool created\n");

    if (!create_vertex_buffer()) {
        fprintf(stderr, "[UIOverlay] Failed to create vertex buffer\n");
        return false;
    }
    printf("[UIOverlay] Vertex buffer created (%zu bytes)\n",
           static_cast<size_t>(vertex_buffer_size_));

    if (!create_pipeline(shader_dir)) {
        fprintf(stderr, "[UIOverlay] Failed to create pipeline\n");
        return false;
    }
    printf("[UIOverlay] Pipeline created\n");

    // Create 1x1 white fallback texture
    uint8_t white[] = {255, 255, 255, 255};
    white_texture_ = register_texture(white, 1, 1, "_white");
    if (white_texture_ == INVALID_TEXTURE) {
        fprintf(stderr, "[UIOverlay] Warning: Failed to create white fallback texture\n");
    } else {
        printf("[UIOverlay] White fallback texture created (handle=%u)\n", white_texture_);
    }

    initialized_ = true;
    printf("[UIOverlay] Initialization complete\n");
    return true;
}

void UIOverlayPipeline::shutdown() {
    if (!initialized_) return;
    printf("[UIOverlay] Shutting down...\n");
    vkDeviceWaitIdle(device_);

    // Destroy textures
    for (auto& t : textures_) {
        if (t.view)   vkDestroyImageView(device_, t.view, nullptr);
        if (t.image)  vkDestroyImage(device_, t.image, nullptr);
        if (t.memory) vkFreeMemory(device_, t.memory, nullptr);
    }
    textures_.clear();

    if (pipeline_)        vkDestroyPipeline(device_, pipeline_, nullptr);
    if (pipeline_layout_) vkDestroyPipelineLayout(device_, pipeline_layout_, nullptr);
    if (desc_pool_)       vkDestroyDescriptorPool(device_, desc_pool_, nullptr);
    if (desc_set_layout_) vkDestroyDescriptorSetLayout(device_, desc_set_layout_, nullptr);
    if (sampler_)         vkDestroySampler(device_, sampler_, nullptr);

    if (vertex_buffer_) vkDestroyBuffer(device_, vertex_buffer_, nullptr);
    if (vertex_memory_) vkFreeMemory(device_, vertex_memory_, nullptr);

    groups_.clear();
    initialized_ = false;
    printf("[UIOverlay] Shutdown complete\n");
}

// ─── Group management ────────────────────────────────────────

OverlayGroupId UIOverlayPipeline::create_group(const std::string& name, int32_t sort_order) {
    auto id = next_group_id_++;
    auto group = std::make_unique<ScreenOverlayGroup>(id, name);
    group->set_sort_order(sort_order);
    printf("[UIOverlay] Created group '%s' (id=%u, sort_order=%d)\n",
           name.c_str(), id, sort_order);
    groups_.push_back(std::move(group));
    return id;
}

ScreenOverlayGroup* UIOverlayPipeline::get_group(OverlayGroupId id) {
    for (auto& g : groups_) {
        if (g->id() == id) return g.get();
    }
    return nullptr;
}

const ScreenOverlayGroup* UIOverlayPipeline::get_group(OverlayGroupId id) const {
    for (const auto& g : groups_) {
        if (g->id() == id) return g.get();
    }
    return nullptr;
}

bool UIOverlayPipeline::remove_group(OverlayGroupId id) {
    auto it = std::find_if(groups_.begin(), groups_.end(),
                           [id](const auto& g) { return g->id() == id; });
    if (it == groups_.end()) return false;
    printf("[UIOverlay] Removed group '%s' (id=%u)\n", (*it)->name().c_str(), id);
    groups_.erase(it);
    return true;
}

// ─── Element convenience ─────────────────────────────────────

OverlayElementId UIOverlayPipeline::add_element(OverlayGroupId group_id,
                                                  const OverlayElement& elem) {
    auto* g = get_group(group_id);
    if (!g) return INVALID_OVERLAY_ELEMENT;
    return g->add_element(elem);
}

bool UIOverlayPipeline::update_element(OverlayGroupId group_id, OverlayElementId elem_id,
                                        const OverlayElement& elem) {
    auto* g = get_group(group_id);
    return g ? g->update_element(elem_id, elem) : false;
}

bool UIOverlayPipeline::remove_element(OverlayGroupId group_id, OverlayElementId elem_id) {
    auto* g = get_group(group_id);
    return g ? g->remove_element(elem_id) : false;
}

// ─── Texture management ──────────────────────────────────────

TextureHandle UIOverlayPipeline::register_texture(const uint8_t* rgba_data,
                                                    uint32_t width, uint32_t height,
                                                    const std::string& name) {
    if (textures_.size() >= config_.max_textures) {
        fprintf(stderr, "[UIOverlay] Max textures reached (%u)\n", config_.max_textures);
        return INVALID_TEXTURE;
    }

    TextureEntry entry;
    entry.width  = width;
    entry.height = height;
    entry.name   = name;

    if (!upload_image(rgba_data, width, height, entry.image, entry.memory)) {
        fprintf(stderr, "[UIOverlay] Failed to upload texture '%s'\n", name.c_str());
        return INVALID_TEXTURE;
    }

    // Image view
    VkImageViewCreateInfo view_info{};
    view_info.sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.image    = entry.image;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format   = VK_FORMAT_R8G8B8A8_UNORM;
    view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    if (vkCreateImageView(device_, &view_info, nullptr, &entry.view) != VK_SUCCESS) {
        fprintf(stderr, "[UIOverlay] Failed to create image view for '%s'\n", name.c_str());
        vkDestroyImage(device_, entry.image, nullptr);
        vkFreeMemory(device_, entry.memory, nullptr);
        return INVALID_TEXTURE;
    }

    // Descriptor set
    entry.desc_set = allocate_texture_descriptor(entry.view);
    if (!entry.desc_set) {
        fprintf(stderr, "[UIOverlay] Failed to allocate descriptor for '%s'\n", name.c_str());
        vkDestroyImageView(device_, entry.view, nullptr);
        vkDestroyImage(device_, entry.image, nullptr);
        vkFreeMemory(device_, entry.memory, nullptr);
        return INVALID_TEXTURE;
    }

    TextureHandle handle = next_texture_handle_++;
    textures_.push_back(entry);
    printf("[UIOverlay] Registered texture '%s' (handle=%u, %ux%u)\n",
           name.c_str(), handle, width, height);
    return handle;
}

bool UIOverlayPipeline::update_texture(TextureHandle handle, const uint8_t* rgba_data,
                                        uint32_t width, uint32_t height) {
    if (handle >= textures_.size()) return false;
    // For simplicity, re-upload (destroy old image, create new)
    auto& entry = textures_[handle];
    vkDeviceWaitIdle(device_);

    if (entry.view)   { vkDestroyImageView(device_, entry.view, nullptr); entry.view = VK_NULL_HANDLE; }
    if (entry.image)  { vkDestroyImage(device_, entry.image, nullptr); entry.image = VK_NULL_HANDLE; }
    if (entry.memory) { vkFreeMemory(device_, entry.memory, nullptr); entry.memory = VK_NULL_HANDLE; }

    if (!upload_image(rgba_data, width, height, entry.image, entry.memory))
        return false;

    entry.width  = width;
    entry.height = height;

    VkImageViewCreateInfo view_info{};
    view_info.sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.image    = entry.image;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format   = VK_FORMAT_R8G8B8A8_UNORM;
    view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    if (vkCreateImageView(device_, &view_info, nullptr, &entry.view) != VK_SUCCESS)
        return false;

    // Update descriptor
    VkDescriptorImageInfo desc_img{};
    desc_img.sampler     = sampler_;
    desc_img.imageView   = entry.view;
    desc_img.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkWriteDescriptorSet write{};
    write.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet          = entry.desc_set;
    write.dstBinding      = 0;
    write.descriptorCount = 1;
    write.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo      = &desc_img;
    vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);

    return true;
}

// ─── Update & Render ─────────────────────────────────────────

void UIOverlayPipeline::update(float screen_width, float screen_height) {
    stats_ = {};
    stats_.total_groups = static_cast<uint32_t>(groups_.size());

    // Sort groups by sort_order
    std::sort(groups_.begin(), groups_.end(),
              [](const auto& a, const auto& b) {
                  return a->sort_order() < b->sort_order();
              });

    // Rebuild dirty batches
    for (auto& g : groups_) {
        if (!g->is_visible()) continue;
        stats_.total_elements += g->element_count();
        if (g->rebuild_if_dirty(screen_width, screen_height)) {
            stats_.batch_rebuilds++;
        }
    }

    // Compute total vertices and upload
    uint32_t total_verts = 0;
    for (const auto& g : groups_) {
        if (g->is_visible()) total_verts += g->vertex_count();
    }

    if (total_verts == 0) return;
    if (total_verts > config_.max_vertices) {
        fprintf(stderr, "[UIOverlay] Warning: vertex count %u exceeds max %u, clamping\n",
                total_verts, config_.max_vertices);
        total_verts = config_.max_vertices;
    }

    // Upload combined vertex data to GPU
    VkDeviceSize upload_size = total_verts * sizeof(UIVertex);
    if (upload_size > vertex_buffer_size_) {
        fprintf(stderr, "[UIOverlay] Warning: upload size exceeds buffer, skipping\n");
        return;
    }

    void* mapped = nullptr;
    vkMapMemory(device_, vertex_memory_, 0, upload_size, 0, &mapped);
    size_t offset = 0;
    for (const auto& g : groups_) {
        if (!g->is_visible() || g->vertex_count() == 0) continue;
        size_t bytes = g->vertex_count() * sizeof(UIVertex);
        if (offset + bytes > vertex_buffer_size_) break;
        memcpy(static_cast<char*>(mapped) + offset, g->vertices().data(), bytes);
        offset += bytes;
    }
    vkUnmapMemory(device_, vertex_memory_);

    stats_.vertices_uploaded = total_verts;
}

void UIOverlayPipeline::render(VkCommandBuffer cmd, VkExtent2D extent) {
    if (!initialized_) return;

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);

    VkViewport viewport{};
    viewport.width    = static_cast<float>(extent.width);
    viewport.height   = static_cast<float>(extent.height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &viewport);

    VkRect2D scissor = {{0, 0}, extent};
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    VkDeviceSize vb_offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &vertex_buffer_, &vb_offset);

    // Draw each group as a separate draw call (different texture binding)
    uint32_t vertex_offset = 0;
    for (const auto& g : groups_) {
        if (!g->is_visible() || g->vertex_count() == 0) continue;

        // Bind texture for this group
        TextureHandle tex = g->texture();
        if (tex == INVALID_TEXTURE) tex = white_texture_;

        if (tex < textures_.size() && textures_[tex].desc_set) {
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    pipeline_layout_, 0, 1,
                                    &textures_[tex].desc_set, 0, nullptr);
        }

        vkCmdDraw(cmd, g->vertex_count(), 1, vertex_offset, 0);
        vertex_offset += g->vertex_count();
        stats_.draw_calls++;
    }
}

// ─── Vulkan resource creation ────────────────────────────────

bool UIOverlayPipeline::create_sampler() {
    VkSamplerCreateInfo info{};
    info.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    info.magFilter    = VK_FILTER_LINEAR;
    info.minFilter    = VK_FILTER_LINEAR;
    info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    info.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_LINEAR;

    return vkCreateSampler(device_, &info, nullptr, &sampler_) == VK_SUCCESS;
}

bool UIOverlayPipeline::create_descriptor_pool() {
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
    pool_size.descriptorCount = config_.max_textures;

    VkDescriptorPoolCreateInfo pool_info{};
    pool_info.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_info.maxSets       = config_.max_textures;
    pool_info.poolSizeCount = 1;
    pool_info.pPoolSizes    = &pool_size;

    return vkCreateDescriptorPool(device_, &pool_info, nullptr, &desc_pool_) == VK_SUCCESS;
}

bool UIOverlayPipeline::create_vertex_buffer() {
    vertex_buffer_size_ = config_.max_vertices * sizeof(UIVertex);
    return create_buffer(vertex_buffer_size_,
                         VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                         vertex_buffer_, vertex_memory_);
}

bool UIOverlayPipeline::create_pipeline(const char* shader_dir) {
    // Reuse the same texture2d shaders (they work for UI: pos+uv+push_constants)
    // For a dedicated UI pipeline, we use per-vertex color instead of push constant tint
    std::string vert_path = std::string(shader_dir) + "/ui_overlay.vert.spv";
    std::string frag_path = std::string(shader_dir) + "/ui_overlay.frag.spv";

    printf("[UIOverlay] Loading shaders: %s, %s\n", vert_path.c_str(), frag_path.c_str());

    VkShaderModule vert = load_shader(vert_path.c_str());
    VkShaderModule frag = load_shader(frag_path.c_str());
    if (!vert || !frag) {
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

    // Vertex input: UIVertex {pos[2], uv[2], color[4]}
    VkVertexInputBindingDescription bind{};
    bind.binding   = 0;
    bind.stride    = sizeof(UIVertex);
    bind.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    VkVertexInputAttributeDescription attrs[3]{};
    attrs[0].location = 0; attrs[0].binding = 0;
    attrs[0].format   = VK_FORMAT_R32G32_SFLOAT;
    attrs[0].offset   = offsetof(UIVertex, pos);
    attrs[1].location = 1; attrs[1].binding = 0;
    attrs[1].format   = VK_FORMAT_R32G32_SFLOAT;
    attrs[1].offset   = offsetof(UIVertex, uv);
    attrs[2].location = 2; attrs[2].binding = 0;
    attrs[2].format   = VK_FORMAT_R32G32B32A32_SFLOAT;
    attrs[2].offset   = offsetof(UIVertex, color);

    VkPipelineVertexInputStateCreateInfo vertex_input{};
    vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertex_input.vertexBindingDescriptionCount   = 1;
    vertex_input.pVertexBindingDescriptions      = &bind;
    vertex_input.vertexAttributeDescriptionCount = 3;
    vertex_input.pVertexAttributeDescriptions    = attrs;

    VkPipelineInputAssemblyStateCreateInfo ia{};
    ia.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo vp{};
    vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.viewportCount = 1;
    vp.scissorCount  = 1;

    VkPipelineRasterizationStateCreateInfo raster{};
    raster.sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode    = VK_CULL_MODE_NONE;
    raster.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth   = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo ds{};
    ds.sType            = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds.depthTestEnable  = VK_FALSE;
    ds.depthWriteEnable = VK_FALSE;

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

    // No push constants — per-vertex color replaces tint
    VkPipelineLayoutCreateInfo layout_info{};
    layout_info.sType          = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout_info.setLayoutCount = 1;
    layout_info.pSetLayouts    = &desc_set_layout_;

    if (vkCreatePipelineLayout(device_, &layout_info, nullptr, &pipeline_layout_) != VK_SUCCESS) {
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
        fprintf(stderr, "[UIOverlay] Failed to create pipeline (VkResult=%d)\n", result);
        return false;
    }

    return true;
}

bool UIOverlayPipeline::upload_image(const uint8_t* data, uint32_t w, uint32_t h,
                                      VkImage& image, VkDeviceMemory& memory) {
    VkDeviceSize size = static_cast<VkDeviceSize>(w) * h * 4;

    // Staging buffer
    VkBuffer staging_buf; VkDeviceMemory staging_mem;
    if (!create_buffer(size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                       staging_buf, staging_mem))
        return false;

    void* mapped;
    vkMapMemory(device_, staging_mem, 0, size, 0, &mapped);
    memcpy(mapped, data, size);
    vkUnmapMemory(device_, staging_mem);

    // Create image
    VkImageCreateInfo img_info{};
    img_info.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    img_info.imageType     = VK_IMAGE_TYPE_2D;
    img_info.format        = VK_FORMAT_R8G8B8A8_UNORM;
    img_info.extent        = {w, h, 1};
    img_info.mipLevels     = 1;
    img_info.arrayLayers   = 1;
    img_info.samples       = VK_SAMPLE_COUNT_1_BIT;
    img_info.tiling        = VK_IMAGE_TILING_OPTIMAL;
    img_info.usage         = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    img_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    if (vkCreateImage(device_, &img_info, nullptr, &image) != VK_SUCCESS) {
        vkDestroyBuffer(device_, staging_buf, nullptr);
        vkFreeMemory(device_, staging_mem, nullptr);
        return false;
    }

    VkMemoryRequirements mem_req;
    vkGetImageMemoryRequirements(device_, image, &mem_req);

    VkMemoryAllocateInfo alloc{};
    alloc.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize  = mem_req.size;
    alloc.memoryTypeIndex = find_memory_type(mem_req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    if (vkAllocateMemory(device_, &alloc, nullptr, &memory) != VK_SUCCESS) {
        vkDestroyImage(device_, image, nullptr);
        vkDestroyBuffer(device_, staging_buf, nullptr);
        vkFreeMemory(device_, staging_mem, nullptr);
        return false;
    }
    vkBindImageMemory(device_, image, memory, 0);

    // Copy via single-time command buffer
    VkCommandBuffer cmd = vk_ctx_->begin_single_time_commands();
    if (!cmd) {
        vkDestroyImage(device_, image, nullptr);
        vkFreeMemory(device_, memory, nullptr);
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
    barrier.image               = image;
    barrier.subresourceRange    = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    barrier.srcAccessMask       = 0;
    barrier.dstAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);

    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent      = {w, h, 1};
    vkCmdCopyBufferToImage(cmd, staging_buf, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    // Transition TRANSFER_DST → SHADER_READ_ONLY
    barrier.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);

    vk_ctx_->end_single_time_commands(cmd);

    vkDestroyBuffer(device_, staging_buf, nullptr);
    vkFreeMemory(device_, staging_mem, nullptr);
    return true;
}

VkDescriptorSet UIOverlayPipeline::allocate_texture_descriptor(VkImageView view) {
    VkDescriptorSetAllocateInfo alloc{};
    alloc.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc.descriptorPool     = desc_pool_;
    alloc.descriptorSetCount = 1;
    alloc.pSetLayouts        = &desc_set_layout_;

    VkDescriptorSet set = VK_NULL_HANDLE;
    if (vkAllocateDescriptorSets(device_, &alloc, &set) != VK_SUCCESS)
        return VK_NULL_HANDLE;

    VkDescriptorImageInfo desc_img{};
    desc_img.sampler     = sampler_;
    desc_img.imageView   = view;
    desc_img.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkWriteDescriptorSet write{};
    write.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet          = set;
    write.dstBinding      = 0;
    write.descriptorCount = 1;
    write.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo      = &desc_img;
    vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);

    return set;
}

VkShaderModule UIOverlayPipeline::load_shader(const char* path) {
    std::ifstream file(path, std::ios::ate | std::ios::binary);
    if (!file.is_open()) {
        fprintf(stderr, "[UIOverlay] Cannot open shader: %s\n", path);
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
    if (vkCreateShaderModule(device_, &info, nullptr, &mod) != VK_SUCCESS)
        fprintf(stderr, "[UIOverlay] Failed to create shader module: %s\n", path);
    return mod;
}

uint32_t UIOverlayPipeline::find_memory_type(uint32_t type_filter, VkMemoryPropertyFlags props) {
    VkPhysicalDeviceMemoryProperties mem_props;
    vkGetPhysicalDeviceMemoryProperties(vk_ctx_->physical_device(), &mem_props);
    for (uint32_t i = 0; i < mem_props.memoryTypeCount; ++i) {
        if ((type_filter & (1 << i)) && (mem_props.memoryTypes[i].propertyFlags & props) == props)
            return i;
    }
    return 0;
}

bool UIOverlayPipeline::create_buffer(VkDeviceSize size, VkBufferUsageFlags usage,
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
