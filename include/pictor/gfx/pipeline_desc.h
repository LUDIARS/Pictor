#pragma once

#include "pictor/gfx/compare_op.h"
#include "pictor/gfx/format.h"
#include "pictor/gfx/shader_bundle_ref.h"

#include <cstdint>

namespace pictor::gfx {

enum class PipelineKind : uint8_t { Graphics = 0, Compute };

enum class PrimitiveTopology : uint8_t {
    TriangleList = 0,
    TriangleStrip,
    LineList,
    PointList,
};

enum class CullMode : uint8_t { Off = 0, Front, Back };

enum class FrontFace : uint8_t { CounterClockwise = 0, Clockwise };

/// Fixed blend presets. Arbitrary factor tables are intentionally not
/// exposed: every backend (including WebGL2) supports these four.
enum class BlendMode : uint8_t {
    Off = 0,
    Alpha,          ///< src * a + dst * (1 - a)
    Premultiplied,  ///< src + dst * (1 - a)
    Additive,       ///< src * a + dst
};

struct VertexBufferLayout {
    uint32_t stride       = 0;
    bool     per_instance = false;
};

struct VertexAttribute {
    uint8_t   location = 0;
    uint8_t   binding  = 0;   ///< index into PipelineDesc::vertex_buffers
    GfxFormat format   = GfxFormat::Rgb32Float;
    uint32_t  offset   = 0;
};

/// Backend-neutral pipeline description. Bind group layouts are not part of
/// it: they come from the shader bundle's reflection (multi-rhi.md §3).
struct PipelineDesc {
    static constexpr uint32_t kMaxColorTargets     = 8;
    static constexpr uint32_t kMaxVertexBuffers    = 8;
    static constexpr uint32_t kMaxVertexAttributes = 16;

    PipelineKind      kind     = PipelineKind::Graphics;
    ShaderBundleRef   shader;
    PrimitiveTopology topology = PrimitiveTopology::TriangleList;

    // Rasteriser
    CullMode  cull_mode  = CullMode::Back;
    FrontFace front_face = FrontFace::CounterClockwise;

    // Depth
    bool      depth_test    = true;
    bool      depth_write   = true;
    CompareOp depth_compare = CompareOp::LessOrEqual;

    // Targets
    uint32_t  color_target_count = 1;
    GfxFormat color_formats[kMaxColorTargets] = {GfxFormat::Rgba8Unorm};
    BlendMode blend[kMaxColorTargets]         = {};
    GfxFormat depth_format = GfxFormat::D32Float;
    uint32_t  sample_count = 1;

    // Vertex input
    uint32_t           vertex_buffer_count = 0;
    VertexBufferLayout vertex_buffers[kMaxVertexBuffers] = {};
    uint32_t           attribute_count = 0;
    VertexAttribute    attributes[kMaxVertexAttributes] = {};

    const char* debug_name = nullptr;  ///< optional, not copied
};

} // namespace pictor::gfx
