#pragma once

/// SHaRC irradiance cache — backend-independent contract (CPU mirrors of the
/// GPU layouts in shaders/sharc/sharc_irradiance.glsl; byte-compatible).
///
/// The irradiance cache is the SHaRC module's path for hosts that keep their
/// own lighting: the host renders its G-buffer and direct light, and the cache
/// returns only the diffuse irradiance arriving at each visible pixel from
/// every bounce. It shares the module's spatial hash, cell storage and
/// accumulation (sharc_common.glsl); rays are traced with hardware ray
/// queries against acceleration structures over the host's own geometry.
///
/// No GPU API or game types.

#include <cstdint>

namespace pictor {

/// Initialisation parameters (change: shutdown, then initialise again).
struct SharcIrradianceConfig {
    uint32_t table_size     = 1u << 20;  ///< hash slots (power of two)
    float    base_cell_size = 0.15f;     ///< level 0 cell edge (m)
    uint32_t level_count    = 12;
    float    level_bias     = 0.13f;     ///< distance -> level: cell ~ distance * bias
    uint32_t stale_frames   = 240;       ///< unsampled this long: the cell is evicted
    uint32_t update_block   = 4;         ///< one ray per update_block^2 screen pixels
};

/// What a hit needs to know about the placement it landed on (std430 uvec4).
/// Indexed by the instance's custom index in the top-level structure.
struct SharcRayInstanceGpu {
    uint32_t first_index;   ///< into the index buffer
    int32_t  vertex_offset; ///< added to every index
    uint32_t surface;       ///< into the surface table
    uint32_t unused;
};
static_assert(sizeof(SharcRayInstanceGpu) == 16, "GLSL SharcRayInstance");

/// A surface as a ray sees it from afar: mean albedo and emitted radiance.
struct SharcRaySurfaceGpu {
    float albedo[4];    ///< linear rgb, w unused
    float emission[4];  ///< linear rgb radiance, w unused
};
static_assert(sizeof(SharcRaySurfaceGpu) == 32, "GLSL SharcRaySurface");

/// A point or spot light (std430, 64 B): inverse-square attenuation windowed
/// to zero at the range (Karis 2013), a smooth cone for spots.
struct SharcIrradianceLightGpu {
    float position_inv_range2[4];   ///< xyz, w = 1 / range^2
    float radiance_kind[4];         ///< linear rgb (irradiance at 1 m, x pi), w = kind (0 spot, else point)
    float direction_spot_scale[4];  ///< xyz where a spot points, w = 1 / (cos inner - cos outer)
    float params[4];                ///< x = -cos outer * spot scale, y host's own, z 1 when it casts shadows, w unused
};
static_assert(sizeof(SharcIrradianceLightGpu) == 64, "GLSL SharcIrradianceLight");

/// The frame as the cache sees it (std140 uniform block, 288 B).
struct alignas(16) SharcIrradianceFrameGpu {
    float inverse_view_projection[16];  ///< clip (Vulkan, depth 0..1) -> world, column-major
    float sun_view_projection[16];      ///< world -> sun shadow clip
    float camera[4];                    ///< eye xyz, w unused
    float screen[4];                    ///< width, height, 1/width, 1/height (the G-buffer's pixels)
    float sun_direction[4];             ///< xyz toward the sun, w 1 when there is a sun
    float sun_radiance[4];              ///< rgb (x pi), w shadow bias
    float sky[4];                       ///< ambient irradiance on a surface facing up (rgb), w = light scale
    float horizon[4];                   ///< ... facing sideways (a miss sees it / pi as radiance)
    float ground[4];                    ///< ... facing down
    float limits[4];                    ///< x sample clamp (irradiance), y history samples, z ray length (m), w strength
    float occlusion[4];                 ///< x ambient-occlusion radius (m, 0 = off), y 1 = shadow rays for the lights that cast them, zw unused
    uint32_t counts[4];                 ///< x light count, y frame index, z update block, w 1 when a sun shadow map is bound
};
static_assert(sizeof(SharcIrradianceFrameGpu) == 288, "GLSL SharcIrradianceFrame");

/// One placement of a mesh range for the acceleration structures.
struct SharcPlacement {
    uint32_t first_index = 0;
    uint32_t index_count = 0;
    int32_t  vertex_offset = 0;
    uint32_t surface = 0;
    float    transform[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};  ///< column-major, affine
};

}  // namespace pictor
