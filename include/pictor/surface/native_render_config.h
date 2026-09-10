#pragma once

#include <cstdint>
#include <cstddef>

namespace pictor {

/// Receives the active native encoder (`MTLRenderCommandEncoder` or
/// `ID3D12GraphicsCommandList`) while its swapchain target is writable.
using NativeDrawCallback = void (*)(void* encoder, uint32_t image_index,
                                    void* user_data);

struct NativeVertex {
    float position[3];
    float color[4];
};

struct NativeMeshView {
    const NativeVertex* vertices = nullptr;
    uint32_t vertex_count = 0;
    const uint32_t* indices = nullptr;
    uint32_t index_count = 0;
};

struct NativeRenderConfig {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t image_count = 3;
    bool vsync = true;
    float clear_color[4] = {0.02f, 0.02f, 0.03f, 1.0f};
    NativeDrawCallback draw = nullptr;
    void* draw_user_data = nullptr;
};

} // namespace pictor
