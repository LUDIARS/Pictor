#include "vk_helpers.h"

#ifdef PICTOR_HAS_VULKAN

#include "pictor/surface/vulkan_context.h"
#include <cstdio>
#include <fstream>

namespace ubg {

VkShaderModule load_shader(VkDevice device, const std::string& path) {
    std::ifstream f(path, std::ios::ate | std::ios::binary);
    if (!f.is_open()) { fprintf(stderr, "[unite_bg] cannot open shader: %s\n", path.c_str()); return VK_NULL_HANDLE; }
    size_t size = static_cast<size_t>(f.tellg());
    if (size == 0 || size % 4 != 0) { fprintf(stderr, "[unite_bg] bad SPIR-V: %s\n", path.c_str()); return VK_NULL_HANDLE; }
    std::vector<char> code(size);
    f.seekg(0);
    f.read(code.data(), static_cast<std::streamsize>(size));
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = size;
    ci.pCode = reinterpret_cast<const uint32_t*>(code.data());
    VkShaderModule m = VK_NULL_HANDLE;
    if (vkCreateShaderModule(device, &ci, nullptr, &m) != VK_SUCCESS) return VK_NULL_HANDLE;
    return m;
}

uint32_t find_memory_type(VkPhysicalDevice phys, uint32_t filter, VkMemoryPropertyFlags props) {
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(phys, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
        if ((filter & (1u << i)) && (mp.memoryTypes[i].propertyFlags & props) == props) return i;
    return UINT32_MAX;
}

// ---------------------------------------------------------------- Buffer
bool Buffer::create(pictor::VulkanContext& ctx, VkDeviceSize sz, VkBufferUsageFlags usage, bool host_visible) {
    VkDevice dev = ctx.device();
    size = sz;
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = sz;
    bi.usage = usage;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(dev, &bi, nullptr, &buffer) != VK_SUCCESS) return false;
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(dev, buffer, &req);
    VkMemoryPropertyFlags props = host_visible
        ? (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
        : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = find_memory_type(ctx.physical_device(), req.memoryTypeBits, props);
    if (ai.memoryTypeIndex == UINT32_MAX) return false;
    if (vkAllocateMemory(dev, &ai, nullptr, &memory) != VK_SUCCESS) return false;
    vkBindBufferMemory(dev, buffer, memory, 0);
    if (host_visible) vkMapMemory(dev, memory, 0, VK_WHOLE_SIZE, 0, &mapped);
    return true;
}

void Buffer::destroy(VkDevice dev) {
    if (mapped) { vkUnmapMemory(dev, memory); mapped = nullptr; }
    if (buffer) vkDestroyBuffer(dev, buffer, nullptr);
    if (memory) vkFreeMemory(dev, memory, nullptr);
    buffer = VK_NULL_HANDLE; memory = VK_NULL_HANDLE;
}

// ---------------------------------------------------------------- Image
bool Image::create(pictor::VulkanContext& ctx, uint32_t w, uint32_t h, VkFormat fmt,
                   VkImageUsageFlags usage, VkImageAspectFlags aspect) {
    VkDevice dev = ctx.device();
    width = w; height = h; format = fmt;
    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = fmt;
    ii.extent = {w, h, 1};
    ii.mipLevels = 1;
    ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = usage;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(dev, &ii, nullptr, &image) != VK_SUCCESS) return false;
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(dev, image, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = find_memory_type(ctx.physical_device(), req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (ai.memoryTypeIndex == UINT32_MAX) return false;
    if (vkAllocateMemory(dev, &ai, nullptr, &memory) != VK_SUCCESS) return false;
    vkBindImageMemory(dev, image, memory, 0);
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = fmt;
    vi.subresourceRange = {aspect, 0, 1, 0, 1};
    return vkCreateImageView(dev, &vi, nullptr, &view) == VK_SUCCESS;
}

void Image::destroy(VkDevice dev) {
    if (view) vkDestroyImageView(dev, view, nullptr);
    if (image) vkDestroyImage(dev, image, nullptr);
    if (memory) vkFreeMemory(dev, memory, nullptr);
    view = VK_NULL_HANDLE; image = VK_NULL_HANDLE; memory = VK_NULL_HANDLE;
}

// ---------------------------------------------------------------- OffscreenTarget
bool OffscreenTarget::create(pictor::VulkanContext& ctx, uint32_t w, uint32_t h, uint32_t count, VkFormat fmt) {
    VkDevice dev = ctx.device();
    width = w; height = h;
    colors.resize(count);
    std::vector<VkAttachmentDescription> atts(count);
    std::vector<VkAttachmentReference> refs(count);
    std::vector<VkImageView> views(count);
    for (uint32_t i = 0; i < count; ++i) {
        if (!colors[i].create(ctx, w, h, fmt,
                              VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                              VK_IMAGE_ASPECT_COLOR_BIT)) return false;
        VkAttachmentDescription& a = atts[i];
        a = {};
        a.format = fmt;
        a.samples = VK_SAMPLE_COUNT_1_BIT;
        a.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        a.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        a.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        a.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        a.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        refs[i] = {i, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        views[i] = colors[i].view;
    }
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = count;
    sub.pColorAttachments = refs.data();

    // 前フレームでの読み取り → 今回の書き込み、今回の書き込み → 後続の読み取り
    VkSubpassDependency deps[2]{};
    deps[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    deps[0].dstSubpass = 0;
    deps[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    deps[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    deps[1].srcSubpass = 0;
    deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    deps[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    deps[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

    VkRenderPassCreateInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rp.attachmentCount = count;
    rp.pAttachments = atts.data();
    rp.subpassCount = 1;
    rp.pSubpasses = &sub;
    rp.dependencyCount = 2;
    rp.pDependencies = deps;
    if (vkCreateRenderPass(dev, &rp, nullptr, &render_pass) != VK_SUCCESS) return false;

    VkFramebufferCreateInfo fb{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fb.renderPass = render_pass;
    fb.attachmentCount = count;
    fb.pAttachments = views.data();
    fb.width = w; fb.height = h; fb.layers = 1;
    return vkCreateFramebuffer(dev, &fb, nullptr, &framebuffer) == VK_SUCCESS;
}

void OffscreenTarget::destroy(VkDevice dev) {
    if (framebuffer) vkDestroyFramebuffer(dev, framebuffer, nullptr);
    if (render_pass) vkDestroyRenderPass(dev, render_pass, nullptr);
    for (auto& c : colors) c.destroy(dev);
    colors.clear();
    framebuffer = VK_NULL_HANDLE; render_pass = VK_NULL_HANDLE;
}

void OffscreenTarget::begin(VkCommandBuffer cmd) const {
    std::vector<VkClearValue> clears(colors.size());
    for (auto& c : clears) c.color = {{0.0f, 0.0f, 0.0f, 0.0f}};
    VkRenderPassBeginInfo bi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    bi.renderPass = render_pass;
    bi.framebuffer = framebuffer;
    bi.renderArea = {{0, 0}, {width, height}};
    bi.clearValueCount = static_cast<uint32_t>(clears.size());
    bi.pClearValues = clears.data();
    vkCmdBeginRenderPass(cmd, &bi, VK_SUBPASS_CONTENTS_INLINE);
    VkViewport vp{0.0f, 0.0f, float(width), float(height), 0.0f, 1.0f};
    VkRect2D sc{{0, 0}, {width, height}};
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &sc);
}

void OffscreenTarget::transition_to_read(pictor::VulkanContext& ctx) const {
    VkCommandBuffer cmd = ctx.begin_single_time_commands();
    for (const auto& c : colors) {
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = c.image;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &b);
    }
    ctx.end_single_time_commands(cmd);
}

// ---------------------------------------------------------------- Pipelines
VkPipeline create_graphics_pipeline(VkDevice device, VkRenderPass rp, VkPipelineLayout layout,
                                    const std::string& vert_spv, const std::string& frag_spv,
                                    const GfxState& st) {
    VkShaderModule vs = load_shader(device, vert_spv);
    VkShaderModule fs = load_shader(device, frag_spv);
    if (!vs || !fs) {
        if (vs) vkDestroyShaderModule(device, vs, nullptr);
        if (fs) vkDestroyShaderModule(device, fs, nullptr);
        return VK_NULL_HANDLE;
    }
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fs;
    stages[1].pName = "main";

    VkPipelineVertexInputStateCreateInfo vin{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vps{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vps.viewportCount = 1;
    vps.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.depthTestEnable = st.depth_test ? VK_TRUE : VK_FALSE;
    ds.depthWriteEnable = st.depth_write ? VK_TRUE : VK_FALSE;
    ds.depthCompareOp = st.depth_op;
    std::vector<VkPipelineColorBlendAttachmentState> cbs(st.color_count);
    for (auto& c : cbs) {
        c = {};
        c.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                           VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    }
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = st.color_count;
    cb.pAttachments = cbs.data();
    VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dy.dynamicStateCount = 2;
    dy.pDynamicStates = dyn;

    VkGraphicsPipelineCreateInfo pi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pi.stageCount = 2;
    pi.pStages = stages;
    pi.pVertexInputState = &vin;
    pi.pInputAssemblyState = &ia;
    pi.pViewportState = &vps;
    pi.pRasterizationState = &rs;
    pi.pMultisampleState = &ms;
    pi.pDepthStencilState = st.has_depth_attachment ? &ds : nullptr;
    pi.pColorBlendState = &cb;
    pi.pDynamicState = &dy;
    pi.layout = layout;
    pi.renderPass = rp;
    VkPipeline p = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pi, nullptr, &p) != VK_SUCCESS) p = VK_NULL_HANDLE;
    vkDestroyShaderModule(device, vs, nullptr);
    vkDestroyShaderModule(device, fs, nullptr);
    return p;
}

VkPipeline create_compute_pipeline(VkDevice device, VkPipelineLayout layout, const std::string& comp_spv) {
    VkShaderModule cs = load_shader(device, comp_spv);
    if (!cs) return VK_NULL_HANDLE;
    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    ci.stage.module = cs;
    ci.stage.pName = "main";
    ci.layout = layout;
    VkPipeline p = VK_NULL_HANDLE;
    if (vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &ci, nullptr, &p) != VK_SUCCESS) p = VK_NULL_HANDLE;
    vkDestroyShaderModule(device, cs, nullptr);
    return p;
}

} // namespace ubg

#endif
