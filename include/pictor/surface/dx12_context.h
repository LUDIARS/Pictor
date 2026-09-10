#pragma once

#include "pictor/surface/frame_result.h"
#include "pictor/surface/native_render_config.h"
#include <vector>

namespace pictor {

/// Native DirectX 12 swapchain and clear-pass renderer. HWND remains host-owned.
class Dx12Context {
public:
    struct State;
    Dx12Context();
    ~Dx12Context();
    Dx12Context(const Dx12Context&) = delete;
    Dx12Context& operator=(const Dx12Context&) = delete;

    bool initialize(void* hwnd, const NativeRenderConfig& config);
    void shutdown();
    FrameResult acquire_frame();
    FrameResult present_frame();
    FrameResult recover_surface(void* hwnd, const NativeRenderConfig& config);
    FrameResult recover_device();
    bool upload_mesh(const NativeMeshView& mesh);
    bool is_initialized() const { return initialized_; }

private:
    State* state_ = nullptr;
    void* hwnd_ = nullptr;
    NativeRenderConfig config_{};
    std::vector<NativeVertex> mesh_vertices_;
    std::vector<uint32_t> mesh_indices_;
    bool initialized_ = false;
};

} // namespace pictor
