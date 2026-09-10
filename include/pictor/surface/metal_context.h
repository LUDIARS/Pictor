#pragma once

#include "pictor/surface/frame_result.h"
#include "pictor/surface/native_render_config.h"
#include <vector>

namespace pictor {

/// Native Metal surface owner. The CAMetalLayer remains host-owned.
class MetalContext {
public:
    struct State;
    MetalContext();
    ~MetalContext();
    MetalContext(const MetalContext&) = delete;
    MetalContext& operator=(const MetalContext&) = delete;

    bool initialize(void* metal_layer, const NativeRenderConfig& config);
    void shutdown();
    FrameResult acquire_frame();
    FrameResult present_frame();
    FrameResult recover_surface(void* metal_layer, const NativeRenderConfig& config);
    FrameResult recover_device();
    bool upload_mesh(const NativeMeshView& mesh);
    bool is_initialized() const { return initialized_; }

private:
    State* state_ = nullptr;
    void* metal_layer_ = nullptr;
    NativeRenderConfig config_{};
    std::vector<NativeVertex> mesh_vertices_;
    std::vector<uint32_t> mesh_indices_;
    bool initialized_ = false;
};

} // namespace pictor
