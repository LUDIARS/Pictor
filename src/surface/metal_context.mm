#include "pictor/surface/metal_context.h"

#if defined(__APPLE__)
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

namespace pictor {

struct MetalContext::State {
    id<MTLDevice> device = nil;
    id<MTLCommandQueue> queue = nil;
    id<MTLRenderPipelineState> pipeline = nil;
    id<MTLBuffer> vertex_buffer = nil;
    id<MTLBuffer> index_buffer = nil;
    uint32_t vertex_count = 0;
    uint32_t index_count = 0;
    id<CAMetalDrawable> drawable = nil;
    id<MTLCommandBuffer> command_buffer = nil;
    FrameStatus completed_status = FrameStatus::Ready;
};

namespace {
MetalContext::State* create_state(void* layer_pointer,
                                  const NativeRenderConfig& config) {
    if (!layer_pointer || config.width == 0 || config.height == 0) return nullptr;
    auto* state = new MetalContext::State();
    state->device = MTLCreateSystemDefaultDevice();
    if (!state->device) { delete state; return nullptr; }
    state->queue = [state->device newCommandQueue];
    if (!state->queue) { delete state; return nullptr; }
    NSString* source = @"#include <metal_stdlib>\nusing namespace metal;\n"
        "struct Input { packed_float3 position; float4 color; };\n"
        "struct V { float4 position [[position]]; float4 color; };\n"
        "vertex V pictor_vertex(uint id [[vertex_id]], const device Input* input [[buffer(0)]]) {\n"
        "V v; v.position=float4(float3(input[id].position),1); v.color=input[id].color; return v; }\n"
        "fragment float4 pictor_fragment(V in [[stage_in]]) { return float4(in.color,1); }\n";
    NSError* error = nil;
    id<MTLLibrary> library = [state->device newLibraryWithSource:source options:nil error:&error];
    if (!library) { delete state; return nullptr; }
    MTLRenderPipelineDescriptor* pipeline_desc = [[MTLRenderPipelineDescriptor alloc] init];
    pipeline_desc.vertexFunction = [library newFunctionWithName:@"pictor_vertex"];
    pipeline_desc.fragmentFunction = [library newFunctionWithName:@"pictor_fragment"];
    pipeline_desc.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    state->pipeline = [state->device newRenderPipelineStateWithDescriptor:pipeline_desc error:&error];
    if (!state->pipeline) { delete state; return nullptr; }
    CAMetalLayer* layer = (__bridge CAMetalLayer*)layer_pointer;
    layer.device = state->device;
    layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
    layer.framebufferOnly = YES;
    layer.maximumDrawableCount = config.image_count < 2 ? 2 :
                                 (config.image_count > 3 ? 3 : config.image_count);
    layer.drawableSize = CGSizeMake(config.width, config.height);
    return state;
}
}

MetalContext::MetalContext() = default;
MetalContext::~MetalContext() { shutdown(); }

bool MetalContext::initialize(void* metal_layer, const NativeRenderConfig& config) {
    if (initialized_) return false;
    State* replacement = create_state(metal_layer, config);
    if (!replacement) return false;
    state_ = replacement;
    metal_layer_ = metal_layer;
    config_ = config;
    initialized_ = true;
    return true;
}

bool MetalContext::upload_mesh(const NativeMeshView& mesh) {
    if (!state_ || !mesh.vertices || mesh.vertex_count == 0) return false;
    id<MTLBuffer> vertices = [state_->device
        newBufferWithBytes:mesh.vertices
        length:sizeof(NativeVertex) * mesh.vertex_count
        options:MTLResourceStorageModeShared];
    if (!vertices) return false;
    id<MTLBuffer> indices = nil;
    if (mesh.index_count > 0) {
        if (!mesh.indices) return false;
        indices = [state_->device newBufferWithBytes:mesh.indices
            length:sizeof(uint32_t) * mesh.index_count
            options:MTLResourceStorageModeShared];
        if (!indices) return false;
    }
    state_->vertex_buffer = vertices;
    state_->index_buffer = indices;
    state_->vertex_count = mesh.vertex_count;
    state_->index_count = mesh.index_count;
    if (mesh.vertices != mesh_vertices_.data()) {
        mesh_vertices_.assign(mesh.vertices, mesh.vertices + mesh.vertex_count);
    }
    if (mesh.index_count == 0) {
        mesh_indices_.clear();
    } else if (mesh.indices != mesh_indices_.data()) {
        mesh_indices_.assign(mesh.indices, mesh.indices + mesh.index_count);
    }
    return true;
}

void MetalContext::shutdown() {
    if (state_ && state_->command_buffer) [state_->command_buffer waitUntilCompleted];
    delete state_;
    state_ = nullptr;
    initialized_ = false;
}

FrameResult MetalContext::acquire_frame() {
    if (!initialized_ || !state_) return {FrameStatus::NotInitialized};
    if (!metal_layer_) return {FrameStatus::SurfaceLost};
    const FrameStatus completed = state_->completed_status;
    if (completed != FrameStatus::Ready) return {completed};
    CAMetalLayer* layer = (__bridge CAMetalLayer*)metal_layer_;
    state_->drawable = [layer nextDrawable];
    if (!state_->drawable) return {FrameStatus::SurfaceLost};
    state_->command_buffer = [state_->queue commandBuffer];
    if (!state_->command_buffer) return {FrameStatus::DeviceLost};

    MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = state_->drawable.texture;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    pass.colorAttachments[0].clearColor = MTLClearColorMake(
        config_.clear_color[0], config_.clear_color[1],
        config_.clear_color[2], config_.clear_color[3]);
    id<MTLRenderCommandEncoder> encoder = [state_->command_buffer renderCommandEncoderWithDescriptor:pass];
    if (!encoder) return {FrameStatus::Error};
    [encoder setRenderPipelineState:state_->pipeline];
    if (state_->vertex_buffer) {
        [encoder setVertexBuffer:state_->vertex_buffer offset:0 atIndex:0];
        if (state_->index_buffer) {
            [encoder drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                indexCount:state_->index_count indexType:MTLIndexTypeUInt32
                indexBuffer:state_->index_buffer indexBufferOffset:0];
        } else {
            [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0
                vertexCount:state_->vertex_count];
        }
    }
    if (config_.draw) {
        config_.draw((__bridge void*)encoder, 0, config_.draw_user_data);
    }
    [encoder endEncoding];
    return {FrameStatus::Ready, 0, false};
}

FrameResult MetalContext::present_frame() {
    if (!initialized_ || !state_) return {FrameStatus::NotInitialized};
    if (!state_->drawable || !state_->command_buffer) return {FrameStatus::Error};
    [state_->command_buffer presentDrawable:state_->drawable];
    [state_->command_buffer commit];
    // Keep recovery lifetime deterministic: no completion handler may retain a
    // raw State pointer after replacement. A future multi-flight owner can hold
    // command buffers explicitly instead of weakening this invariant.
    [state_->command_buffer waitUntilCompleted];
    if (state_->command_buffer.status == MTLCommandBufferStatusError) {
        state_->completed_status = FrameStatus::DeviceLost;
        state_->drawable = nil;
        state_->command_buffer = nil;
        return {FrameStatus::DeviceLost};
    }
    state_->drawable = nil;
    state_->command_buffer = nil;
    return {FrameStatus::Ready, 0, false};
}

FrameResult MetalContext::recover_surface(void* metal_layer,
                                          const NativeRenderConfig& config) {
    State* replacement = create_state(metal_layer, config);
    if (!replacement) return {metal_layer ? FrameStatus::Error : FrameStatus::SurfaceLost};
    State* retired = state_;
    state_ = replacement;
    metal_layer_ = metal_layer;
    config_ = config;
    initialized_ = true;
    if (!mesh_vertices_.empty()) {
        const NativeMeshView retained{mesh_vertices_.data(),
            static_cast<uint32_t>(mesh_vertices_.size()), mesh_indices_.data(),
            static_cast<uint32_t>(mesh_indices_.size())};
        if (!upload_mesh(retained)) {
            state_ = retired;
            delete replacement;
            return {FrameStatus::Error};
        }
    }
    if (retired && retired->command_buffer) [retired->command_buffer waitUntilCompleted];
    delete retired;
    return {FrameStatus::Ready};
}

FrameResult MetalContext::recover_device() {
    return recover_surface(metal_layer_, config_);
}

} // namespace pictor
#else
namespace pictor {
MetalContext::MetalContext() = default;
MetalContext::~MetalContext() = default;
bool MetalContext::initialize(void*, const NativeRenderConfig&) { return false; }
bool MetalContext::upload_mesh(const NativeMeshView&) { return false; }
void MetalContext::shutdown() {}
FrameResult MetalContext::acquire_frame() { return {FrameStatus::NotInitialized}; }
FrameResult MetalContext::present_frame() { return {FrameStatus::NotInitialized}; }
FrameResult MetalContext::recover_surface(void*, const NativeRenderConfig&) { return {FrameStatus::Error}; }
FrameResult MetalContext::recover_device() { return {FrameStatus::Error}; }
}
#endif
