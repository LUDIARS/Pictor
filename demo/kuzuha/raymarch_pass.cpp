#include "raymarch_pass.h"
#include "../fbx_viewer/vk_buffer_util.h"
#include <cstring>
#include <stdexcept>

namespace pictor_kuzuha {
using namespace pictor_fbx_viewer;
/// @implements SPEC-PC-KUZUHA-RAYMARCH
bool RaymarchPass::create(const CreateInfo& ci,const std::vector<GpuPnPatch>& patches) {
    if (device_ || patches.empty()) return false;
    VkPhysicalDeviceFeatures features{};vkGetPhysicalDeviceFeatures(ci.physical_device,&features);
    if (!features.fragmentStoresAndAtomics) return false;
    device_=ci.device;
    const auto host=VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    const auto size=patches.size()*sizeof(GpuPnPatch);
    if (!create_buffer(device_,ci.physical_device,size,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,host,buffer_,memory_) ||
        !create_buffer(device_,ci.physical_device,16,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,host,counters_,counters_memory_)) {
        destroy();return false;
    }
    void* mapped=nullptr;
    if (vkMapMemory(device_,memory_,0,size,0,&mapped)!=VK_SUCCESS) { destroy();return false; }
    mapped_patches_=static_cast<GpuPnPatch*>(mapped);patch_count_=patches.size();
    std::memcpy(mapped,patches.data(),size);
    if (vkMapMemory(device_,counters_memory_,0,16,0,&mapped)!=VK_SUCCESS) { destroy();return false; }
    mapped_counters_=static_cast<uint32_t*>(mapped);std::memset(mapped_counters_,0,16);
    VkDescriptorSetLayoutBinding bindings[2]{};
    for (uint32_t i=0;i<2;++i) {
        bindings[i].binding=i;bindings[i].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[i].descriptorCount=1;bindings[i].stageFlags=VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    bindings[0].stageFlags|=VK_SHADER_STAGE_VERTEX_BIT;
    VkDescriptorSetLayoutCreateInfo dci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dci.bindingCount=2;dci.pBindings=bindings;
    if (vkCreateDescriptorSetLayout(device_,&dci,nullptr,&patch_layout_)!=VK_SUCCESS) { destroy();return false; }
    VkDescriptorPoolSize pool_size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,2};
    VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pci.maxSets=1;pci.poolSizeCount=1;pci.pPoolSizes=&pool_size;
    if (vkCreateDescriptorPool(device_,&pci,nullptr,&pool_)!=VK_SUCCESS) { destroy();return false; }
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool=pool_;ai.descriptorSetCount=1;ai.pSetLayouts=&patch_layout_;
    if (vkAllocateDescriptorSets(device_,&ai,&set_)!=VK_SUCCESS) { destroy();return false; }
    VkDescriptorBufferInfo buffers[2]{{buffer_,0,size},{counters_,0,16}};
    VkWriteDescriptorSet writes[2]{};
    for (uint32_t i=0;i<2;++i) {
        writes[i].sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;writes[i].dstSet=set_;writes[i].dstBinding=i;
        writes[i].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;writes[i].descriptorCount=1;writes[i].pBufferInfo=&buffers[i];
    }
    vkUpdateDescriptorSets(device_,2,writes,0,nullptr);
    const VkDescriptorSetLayout layouts[]{ci.scene_layout,ci.texture_layout,patch_layout_};
    VkPushConstantRange range{VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT,0,sizeof(RaymarchParameters)};
    VkPipelineLayoutCreateInfo lci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    lci.setLayoutCount=3;lci.pSetLayouts=layouts;lci.pushConstantRangeCount=1;lci.pPushConstantRanges=&range;
    if (vkCreatePipelineLayout(device_,&lci,nullptr,&layout_)!=VK_SUCCESS) { destroy();return false; }
    const auto vs=load_shader_spv(device_,ci.shader_dir+"/pn_ray.vert.spv");
    const auto fs=load_shader_spv(device_,ci.shader_dir+"/pn_ray.frag.spv");
    if (!vs || !fs) {
        if (vs) vkDestroyShaderModule(device_,vs,nullptr);
        if (fs) vkDestroyShaderModule(device_,fs,nullptr);
        destroy();return false;
    }
    VkPipelineShaderStageCreateInfo stages[2]{};
    for (auto& s:stages) { s.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;s.pName="main"; }
    stages[0].stage=VK_SHADER_STAGE_VERTEX_BIT;stages[0].module=vs;
    stages[1].stage=VK_SHADER_STAGE_FRAGMENT_BIT;stages[1].module=fs;
    VkPipelineVertexInputStateCreateInfo vertex{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    assembly.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewport.viewportCount=1;viewport.scissorCount=1;
    VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.polygonMode=VK_POLYGON_MODE_FILL;raster.cullMode=VK_CULL_MODE_NONE;raster.lineWidth=1;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo depth{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    depth.depthTestEnable=VK_TRUE;depth.depthWriteEnable=VK_TRUE;depth.depthCompareOp=VK_COMPARE_OP_LESS;
    VkPipelineColorBlendAttachmentState attachment{};attachment.colorWriteMask=15;
    VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blend.attachmentCount=1;blend.pAttachments=&attachment;
    VkDynamicState states[]{VK_DYNAMIC_STATE_VIEWPORT,VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic.dynamicStateCount=2;dynamic.pDynamicStates=states;
    VkGraphicsPipelineCreateInfo pi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pi.stageCount=2;pi.pStages=stages;pi.pVertexInputState=&vertex;pi.pInputAssemblyState=&assembly;
    pi.pViewportState=&viewport;pi.pRasterizationState=&raster;pi.pMultisampleState=&ms;
    pi.pDepthStencilState=&depth;pi.pColorBlendState=&blend;pi.pDynamicState=&dynamic;
    pi.layout=layout_;pi.renderPass=ci.render_pass;
    const auto result=vkCreateGraphicsPipelines(device_,VK_NULL_HANDLE,1,&pi,nullptr,&pipeline_);
    vkDestroyShaderModule(device_,vs,nullptr);vkDestroyShaderModule(device_,fs,nullptr);
    if (result!=VK_SUCCESS) { destroy();return false; }
    return true;
}
/// @implements SPEC-PC-KUZUHA-RAYMARCH
void RaymarchPass::destroy() {
    if (!device_) return;
    vkDeviceWaitIdle(device_);
    if (mapped_counters_) vkUnmapMemory(device_,counters_memory_);
    if (mapped_patches_) vkUnmapMemory(device_,memory_);
    mapped_patches_=nullptr;patch_count_=0;
    mapped_counters_=nullptr;
    if (pipeline_) vkDestroyPipeline(device_,pipeline_,nullptr);
    if (layout_) vkDestroyPipelineLayout(device_,layout_,nullptr);
    if (pool_) vkDestroyDescriptorPool(device_,pool_,nullptr);
    if (patch_layout_) vkDestroyDescriptorSetLayout(device_,patch_layout_,nullptr);
    if (buffer_) vkDestroyBuffer(device_,buffer_,nullptr);
    if (counters_) vkDestroyBuffer(device_,counters_,nullptr);
    if (memory_) vkFreeMemory(device_,memory_,nullptr);
    if (counters_memory_) vkFreeMemory(device_,counters_memory_,nullptr);
    pipeline_={};layout_={};pool_={};patch_layout_={};set_={};
    buffer_={};counters_={};memory_={};counters_memory_={};device_={};
}
/// @implements SPEC-PC-KUZUHA-RAYMARCH
uint32_t RaymarchPass::begin_frame() {
    const uint32_t unresolved=mapped_counters_[0];std::memset(mapped_counters_,0,16);return unresolved;
}
/// @implements SPEC-PC-POLYNOMIAL-MOTION
void RaymarchPass::update_patch(uint32_t index,const GpuPnPatch& patch) {
    if (index>=patch_count_ || !mapped_patches_) throw std::out_of_range("Polynomial GPU patch update");
    mapped_patches_[index]=patch;
}
/// @implements SPEC-PC-KUZUHA-RAYMARCH
void RaymarchPass::finish_frame(VkCommandBuffer cmd) const {
    VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    barrier.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&barrier,0,nullptr,0,nullptr);
}
/// @implements SPEC-PC-KUZUHA-RAYMARCH
void RaymarchPass::bind(VkCommandBuffer cmd,VkDescriptorSet scene,const RaymarchParameters& parameters) const {
    vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline_);
    vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,layout_,0,1,&scene,0,nullptr);
    vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,layout_,2,1,&set_,0,nullptr);
    vkCmdPushConstants(cmd,layout_,VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT,0,sizeof(parameters),&parameters);
}
/// @implements SPEC-PC-KUZUHA-RAYMARCH
void RaymarchPass::draw(VkCommandBuffer cmd,VkDescriptorSet texture,uint32_t first_patch,uint32_t count) const {
    vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,layout_,1,1,&texture,0,nullptr);
    vkCmdDraw(cmd,6,count,0,first_patch);
}
}
