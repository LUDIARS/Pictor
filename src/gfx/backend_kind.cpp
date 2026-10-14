#include "pictor/gfx/backend_kind.h"

namespace pictor::gfx {

const char* backend_kind_name(BackendKind kind) noexcept {
    switch (kind) {
        case BackendKind::Null:   return "null";
        case BackendKind::Vulkan: return "vulkan";
        case BackendKind::D3D12:  return "d3d12";
        case BackendKind::Metal:  return "metal";
        case BackendKind::WebGPU: return "webgpu";
        case BackendKind::WebGL2: return "webgl2";
        case BackendKind::Count_: break;
    }
    return "unknown";
}

} // namespace pictor::gfx
