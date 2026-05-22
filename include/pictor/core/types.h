#pragma once

#include <cstdint>
#include <cstddef>
#include <limits>
#include <vector>

// ============================================================
// Cache-Line Alignment Configuration
//
// PICTOR_CACHE_LINE_SIZE controls the alignment of hot data structures
// for Data-Oriented Design (DoD) cache optimization.
//
// Typical values:
//   64  — x86/x64, Apple Silicon (default)
//   32  — some ARM/embedded
//   128 — some server CPUs (Intel Ice Lake L2)
//
// Override via compiler define: -DPICTOR_CACHE_LINE_SIZE=32
// Set to 0 to disable cache-line alignment entirely.
// ============================================================

#ifndef PICTOR_CACHE_LINE_SIZE
#define PICTOR_CACHE_LINE_SIZE 64
#endif

#if PICTOR_CACHE_LINE_SIZE > 0
#define PICTOR_CACHE_ALIGN alignas(PICTOR_CACHE_LINE_SIZE)
#else
#define PICTOR_CACHE_ALIGN
#endif

// Suppress MSVC C4324 "structure was padded due to alignment specifier"
// This is expected and intentional for DoD cache optimization.
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4324)
#endif

namespace pictor {

// ============================================================
// Primitive Types
// ============================================================

struct float3 {
    float x = 0.0f, y = 0.0f, z = 0.0f;

    float3() = default;
    float3(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}

    float3 operator+(const float3& o) const { return {x + o.x, y + o.y, z + o.z}; }
    float3 operator-(const float3& o) const { return {x - o.x, y - o.y, z - o.z}; }
    float3 operator*(float s) const { return {x * s, y * s, z * s}; }
};

struct float4 {
    float x = 0.0f, y = 0.0f, z = 0.0f, w = 0.0f;
};

struct PICTOR_CACHE_ALIGN float4x4 {
    float m[4][4] = {};

    static float4x4 identity() {
        float4x4 r{};
        r.m[0][0] = r.m[1][1] = r.m[2][2] = r.m[3][3] = 1.0f;
        return r;
    }

    void set_translation(float x, float y, float z) {
        m[3][0] = x; m[3][1] = y; m[3][2] = z;
    }

    float3 get_translation() const {
        return {m[3][0], m[3][1], m[3][2]};
    }
};

static_assert(sizeof(float4x4) == 64, "float4x4 must be 64 bytes (1 cache line)");

// ============================================================
// AABB
// ============================================================

struct AABB {
    float3 min;
    float3 max;

    bool intersects(const AABB& other) const {
        return (min.x <= other.max.x && max.x >= other.min.x) &&
               (min.y <= other.max.y && max.y >= other.min.y) &&
               (min.z <= other.max.z && max.z >= other.min.z);
    }

    AABB merge(const AABB& other) const {
        AABB result;
        result.min.x = (min.x < other.min.x) ? min.x : other.min.x;
        result.min.y = (min.y < other.min.y) ? min.y : other.min.y;
        result.min.z = (min.z < other.min.z) ? min.z : other.min.z;
        result.max.x = (max.x > other.max.x) ? max.x : other.max.x;
        result.max.y = (max.y > other.max.y) ? max.y : other.max.y;
        result.max.z = (max.z > other.max.z) ? max.z : other.max.z;
        return result;
    }

    float3 center() const {
        return {(min.x + max.x) * 0.5f,
                (min.y + max.y) * 0.5f,
                (min.z + max.z) * 0.5f};
    }

    float surface_area() const {
        float dx = max.x - min.x;
        float dy = max.y - min.y;
        float dz = max.z - min.z;
        return 2.0f * (dx * dy + dy * dz + dz * dx);
    }
};

static_assert(sizeof(AABB) == 24, "AABB must be 24 bytes");

// ============================================================
// Handle Types
// ============================================================

using ObjectId       = uint32_t;
using MeshHandle     = uint32_t;
using MaterialHandle = uint32_t;
using ShaderHandle   = uint32_t;
using TextureHandle  = uint32_t;
using PoolId         = uint32_t;

constexpr ObjectId INVALID_OBJECT_ID = std::numeric_limits<uint32_t>::max();
constexpr MeshHandle INVALID_MESH    = std::numeric_limits<uint32_t>::max();
constexpr MaterialHandle INVALID_MATERIAL = std::numeric_limits<uint32_t>::max();
constexpr TextureHandle INVALID_TEXTURE   = std::numeric_limits<uint32_t>::max();
constexpr ShaderHandle INVALID_SHADER     = std::numeric_limits<uint32_t>::max();

// ============================================================
// Texture Format
// ============================================================

enum class TextureFormat : uint8_t {
    RGBA8_UNORM  = 0,
    RGBA8_SRGB   = 1,
    RGBA16_FLOAT = 2,
    RGBA32_FLOAT = 3,
    R8_UNORM     = 4,
    RG8_UNORM    = 5,
    BC1_UNORM    = 6,  // DXT1
    BC3_UNORM    = 7,  // DXT5
    BC5_UNORM    = 8,  // Normal map
    BC7_UNORM    = 9,
    DEPTH_32F    = 10,
    DEPTH_24_STENCIL_8 = 11
};

enum class TextureType : uint8_t {
    TEXTURE_2D       = 0,
    TEXTURE_3D       = 1,
    TEXTURE_CUBE     = 2,
    TEXTURE_2D_ARRAY = 3
};

// ============================================================
// Vertex Attribute
// ============================================================

enum class VertexAttributeType : uint8_t {
    FLOAT     = 0,
    FLOAT2    = 1,
    FLOAT3    = 2,
    FLOAT4    = 3,
    UINT32    = 4,
    INT32     = 5,
    UNORM8X4  = 6,  // Packed color
    HALF2     = 7,
    HALF4     = 8,
    UINT32X4  = 9   // uvec4 — skinning joint indices 等
};

enum class VertexSemantic : uint8_t {
    POSITION    = 0,
    NORMAL      = 1,
    TANGENT     = 2,
    TEXCOORD0   = 3,
    TEXCOORD1   = 4,
    COLOR0      = 5,
    JOINTS      = 6,
    WEIGHTS     = 7,
    CUSTOM0     = 8,
    CUSTOM1     = 9,
    CUSTOM2     = 10,
    CUSTOM3     = 11
};

struct VertexAttribute {
    VertexSemantic    semantic;
    VertexAttributeType type;
    uint16_t          offset = 0;  // Byte offset within vertex
};

/// Returns byte size of a VertexAttributeType
inline size_t vertex_attribute_size(VertexAttributeType type) {
    switch (type) {
        case VertexAttributeType::FLOAT:    return 4;
        case VertexAttributeType::FLOAT2:   return 8;
        case VertexAttributeType::FLOAT3:   return 12;
        case VertexAttributeType::FLOAT4:   return 16;
        case VertexAttributeType::UINT32:   return 4;
        case VertexAttributeType::INT32:    return 4;
        case VertexAttributeType::UNORM8X4: return 4;
        case VertexAttributeType::HALF2:    return 4;
        case VertexAttributeType::HALF4:    return 8;
        case VertexAttributeType::UINT32X4: return 16;
        default: return 0;
    }
}

// ============================================================
// VertexLayout — mesh 駆動の頂点入力レイアウト
// ============================================================
// 1 binding ぶんの頂点フォーマット記述 (attribute 集合 + stride)。
//
//   attributes : `VertexAttribute[]` (semantic / type / byte offset)
//   stride     : 1 頂点のバイト幅。 0 のとき `computed_stride()` が
//                attribute から自動算出する (= 末尾 attribute の
//                offset + size)。
//
// 元は `data/vertex_data_uploader.h` の mesh 登録専用構造体だったが、
// `spec/rendering-extensibility-design.md` §6.2 で「カスタムシェーダ
// pipeline の頂点入力レイアウト」 にも使うため `core/types.h` へ昇格した。
// `empty()` が真なら頂点入力空 = `gl_VertexIndex` 駆動のフォールバック。
// Vulkan の `VkVertexInput*Description` への変換は `shader/vertex_layout.h`
// (Vulkan 依存ヘッダ) が担う — 本構造体自体は Vulkan に非依存。
//
// `attributes[i]` の `VertexSemantic` は SPIR-V の `layout(location=N)` と
// 突き合わせる必要があるが、 §6.2 phase 2 では「def に明示記述」 のみ。
// location は attribute の登録順 (index) をそのまま割り当てる
// (SPIR-V reflection は将来)。

struct VertexLayout {
    std::vector<VertexAttribute> attributes;
    uint32_t stride = 0;  // Bytes per vertex. 0 = auto-compute from attributes.

    bool empty() const { return attributes.empty(); }

    /// Compute stride from attributes if not set.
    /// stride が明示されていればそれをそのまま返す。
    uint32_t computed_stride() const {
        if (stride > 0) return stride;
        uint32_t max_end = 0;
        for (const auto& attr : attributes) {
            const uint32_t end = static_cast<uint32_t>(attr.offset) +
                static_cast<uint32_t>(vertex_attribute_size(attr.type));
            if (end > max_end) max_end = end;
        }
        return max_end;
    }
};

// ============================================================
// Object Flags (§3.3)
// ============================================================

namespace ObjectFlags {
    constexpr uint16_t STATIC         = 1 << 0;
    constexpr uint16_t DYNAMIC        = 1 << 1;
    constexpr uint16_t GPU_DRIVEN     = 1 << 2;
    constexpr uint16_t CAST_SHADOW    = 1 << 3;
    constexpr uint16_t RECEIVE_SHADOW = 1 << 4;
    constexpr uint16_t TRANSPARENT    = 1 << 5;
    constexpr uint16_t TWO_SIDED      = 1 << 6;
    constexpr uint16_t INSTANCED      = 1 << 7;
    constexpr uint16_t LAYER_MASK     = 0x0300; // bits 8-9
    constexpr uint16_t RESERVED_MASK  = 0xFC00; // bits 10-15

    inline uint16_t layer(uint16_t flags) { return (flags & LAYER_MASK) >> 8; }
    inline uint16_t set_layer(uint16_t flags, uint16_t layer) {
        return (flags & ~LAYER_MASK) | ((layer & 0x3) << 8);
    }
}

// ============================================================
// Object Descriptor
// ============================================================

struct ObjectDescriptor {
    MeshHandle     mesh       = INVALID_MESH;
    MaterialHandle material   = INVALID_MATERIAL;
    float4x4       transform  = float4x4::identity();
    AABB           bounds     = {};
    uint16_t       flags      = ObjectFlags::DYNAMIC;
    uint64_t       shaderKey  = 0;
    uint32_t       materialKey = 0;
    uint8_t        lodLevel   = 0;
    /// Visus CUSTOM kind 用のシェーダ override
    /// (`spec/rendering-extensibility-design.md` §2 phase 1)。
    /// INVALID_SHADER 以外なら、 そのオブジェクトは固定 PBR 経路ではなく
    /// `ShaderRegistry` の登録済みカスタムシェーダ pipeline で描画される。
    ShaderHandle   customShader = INVALID_SHADER;
};

// ============================================================
// Shader Key — custom-shader identification
// ============================================================
// `ObjectDescriptor::shaderKey` (uint64) はバッチ整列キーの一部であり、
// SoA stream を増やさずに「カスタムシェーダ識別」を運ぶ受け皿になる。
// Visus CUSTOM kind の object は上位 32bit に CUSTOM フラグ + ShaderHandle
// を埋め、 バッチビルダ / レンダラはそれを見て PBR 経路をバイパスする。
//
//   bit 63       : CUSTOM_SHADER フラグ
//   bit 32..62   : ShaderHandle (31bit、 INVALID_SHADER は格納しない)
//   bit 0..31    : 既存の用途 (material/pass バリアント等) のため温存
//
// (`spec/rendering-extensibility-design.md` §2 phase 1)。

namespace ShaderKey {
    constexpr uint64_t CUSTOM_FLAG  = 1ULL << 63;
    constexpr uint64_t HANDLE_SHIFT = 32;
    constexpr uint64_t HANDLE_MASK  = 0x7FFFFFFFULL << HANDLE_SHIFT;

    /// 既存 shaderKey にカスタムシェーダ識別ビットを重ねる (下位 32bit は保持)。
    inline uint64_t with_custom_shader(uint64_t base, ShaderHandle shader) {
        if (shader == INVALID_SHADER) return base;
        return (base & 0xFFFFFFFFULL) | CUSTOM_FLAG |
               ((static_cast<uint64_t>(shader) & 0x7FFFFFFFULL) << HANDLE_SHIFT);
    }
    inline bool is_custom(uint64_t key) { return (key & CUSTOM_FLAG) != 0; }
    inline ShaderHandle custom_shader(uint64_t key) {
        if (!is_custom(key)) return INVALID_SHADER;
        return static_cast<ShaderHandle>((key & HANDLE_MASK) >> HANDLE_SHIFT);
    }
}

// ============================================================
// Render Batch
// ============================================================

struct RenderBatch {
    uint64_t sortKey       = 0;
    uint32_t startIndex    = 0;
    uint32_t count         = 0;
    uint64_t shaderKey     = 0;
    uint32_t materialKey   = 0;
    MeshHandle mesh        = INVALID_MESH;
};

// ============================================================
// Draw Command (Vulkan-compatible)
// ============================================================

struct DrawIndexedIndirectCommand {
    uint32_t indexCount    = 0;
    uint32_t instanceCount = 0;
    uint32_t firstIndex    = 0;
    int32_t  vertexOffset  = 0;
    uint32_t firstInstance = 0;
};

// ============================================================
// Frustum Planes
// ============================================================

struct Plane {
    float3 normal;
    float  distance = 0.0f;
};

struct Frustum {
    Plane planes[6]; // left, right, bottom, top, near, far

    bool test_aabb(const AABB& aabb) const {
        for (int i = 0; i < 6; ++i) {
            float3 p;
            p.x = (planes[i].normal.x > 0) ? aabb.max.x : aabb.min.x;
            p.y = (planes[i].normal.y > 0) ? aabb.max.y : aabb.min.y;
            p.z = (planes[i].normal.z > 0) ? aabb.max.z : aabb.min.z;
            float dot = planes[i].normal.x * p.x +
                        planes[i].normal.y * p.y +
                        planes[i].normal.z * p.z;
            if (dot + planes[i].distance < 0) return false;
        }
        return true;
    }
};

// ============================================================
// Enums
// ============================================================

enum class PoolType : uint8_t {
    STATIC     = 0,
    DYNAMIC    = 1,
    GPU_DRIVEN = 2
};

enum class RenderingPath : uint8_t {
    FORWARD      = 0,
    FORWARD_PLUS = 1,
    DEFERRED     = 2,
    HYBRID       = 3
};

enum class PassType : uint8_t {
    DEPTH_ONLY   = 0,
    OPAQUE       = 1,
    TRANSPARENT  = 2,
    SHADOW       = 3,
    POST_PROCESS = 4,
    COMPUTE      = 5,
    CUSTOM       = 6
};

enum class SortMode : uint8_t {
    FRONT_TO_BACK = 0,
    BACK_TO_FRONT = 1,
    NONE          = 2
};

enum class OverlayMode : uint8_t {
    OFF      = 0,
    MINIMAL  = 1,
    STANDARD = 2,
    DETAILED = 3,
    TIMELINE = 4
};

enum class UpdateStrategy : uint8_t {
    NONE             = 0,
    CPU_PARALLEL     = 1,  // Level 1
    CPU_PARALLEL_NT  = 2,  // Level 1 + Level 2
    GPU_COMPUTE      = 3   // Level 3
};

/// Shadow filtering mode for shadow map sampling
enum class ShadowFilterMode : uint8_t {
    NONE = 0,   // Hard shadows (single sample)
    PCF  = 1,   // Percentage Closer Filtering (uniform soft shadow)
    PCSS = 2    // Percentage Closer Soft Shadows (contact-hardening)
};

} // namespace pictor

#ifdef _MSC_VER
#pragma warning(pop)
#endif
