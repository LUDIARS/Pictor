#pragma once

#include "pictor/core/types.h"
#include <cstdint>
#include <cmath>
#include <vector>
#include <string>
#include <limits>

namespace pictor {

// ============================================================
// Animation Handle Types
// ============================================================

using AnimationClipHandle  = uint32_t;
using SkeletonHandle       = uint32_t;
using AnimationStateHandle = uint32_t;

constexpr AnimationClipHandle  INVALID_ANIMATION_CLIP  = std::numeric_limits<uint32_t>::max();
constexpr SkeletonHandle       INVALID_SKELETON        = std::numeric_limits<uint32_t>::max();
constexpr AnimationStateHandle INVALID_ANIMATION_STATE = std::numeric_limits<uint32_t>::max();

// ============================================================
// Quaternion (for rotation interpolation)
// ============================================================

struct Quaternion {
    float x = 0.0f, y = 0.0f, z = 0.0f, w = 1.0f;

    Quaternion() = default;
    Quaternion(float x_, float y_, float z_, float w_) : x(x_), y(y_), z(z_), w(w_) {}

    static Quaternion identity() { return {0.0f, 0.0f, 0.0f, 1.0f}; }

    float length_sq() const { return x * x + y * y + z * z + w * w; }
    float length() const { return std::sqrt(length_sq()); }

    Quaternion normalized() const {
        float len = length();
        if (len < 1e-8f) return identity();
        float inv = 1.0f / len;
        return {x * inv, y * inv, z * inv, w * inv};
    }

    Quaternion conjugate() const { return {-x, -y, -z, w}; }

    Quaternion operator*(const Quaternion& q) const {
        return {
            w * q.x + x * q.w + y * q.z - z * q.y,
            w * q.y - x * q.z + y * q.w + z * q.x,
            w * q.z + x * q.y - y * q.x + z * q.w,
            w * q.w - x * q.x - y * q.y - z * q.z
        };
    }

    /// Convert quaternion to 4x4 rotation matrix
    float4x4 to_matrix() const {
        float4x4 m = float4x4::identity();
        float xx = x * x, yy = y * y, zz = z * z;
        float xy = x * y, xz = x * z, yz = y * z;
        float wx = w * x, wy = w * y, wz = w * z;
        m.m[0][0] = 1.0f - 2.0f * (yy + zz);
        m.m[0][1] = 2.0f * (xy + wz);
        m.m[0][2] = 2.0f * (xz - wy);
        m.m[1][0] = 2.0f * (xy - wz);
        m.m[1][1] = 1.0f - 2.0f * (xx + zz);
        m.m[1][2] = 2.0f * (yz + wx);
        m.m[2][0] = 2.0f * (xz + wy);
        m.m[2][1] = 2.0f * (yz - wx);
        m.m[2][2] = 1.0f - 2.0f * (xx + yy);
        return m;
    }

    /// Create from axis-angle
    static Quaternion from_axis_angle(const float3& axis, float angle_rad) {
        float half = angle_rad * 0.5f;
        float s = std::sin(half);
        float c = std::cos(half);
        return {axis.x * s, axis.y * s, axis.z * s, c};
    }

    /// Create from Euler angles (XYZ order)
    static Quaternion from_euler(float pitch, float yaw, float roll) {
        float cy = std::cos(yaw * 0.5f),   sy = std::sin(yaw * 0.5f);
        float cp = std::cos(pitch * 0.5f),  sp = std::sin(pitch * 0.5f);
        float cr = std::cos(roll * 0.5f),   sr = std::sin(roll * 0.5f);
        return {
            sr * cp * cy - cr * sp * sy,
            cr * sp * cy + sr * cp * sy,
            cr * cp * sy - sr * sp * cy,
            cr * cp * cy + sr * sp * sy
        };
    }
};

/// Spherical linear interpolation
inline Quaternion slerp(const Quaternion& a, const Quaternion& b, float t) {
    float dot = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    Quaternion b2 = b;
    if (dot < 0.0f) {
        dot = -dot;
        b2 = {-b.x, -b.y, -b.z, -b.w};
    }
    if (dot > 0.9995f) {
        // Linear interpolation for very close quaternions
        return Quaternion{
            a.x + t * (b2.x - a.x),
            a.y + t * (b2.y - a.y),
            a.z + t * (b2.z - a.z),
            a.w + t * (b2.w - a.w)
        }.normalized();
    }
    float theta = std::acos(dot);
    float sin_theta = std::sin(theta);
    float wa = std::sin((1.0f - t) * theta) / sin_theta;
    float wb = std::sin(t * theta) / sin_theta;
    return Quaternion{
        wa * a.x + wb * b2.x,
        wa * a.y + wb * b2.y,
        wa * a.z + wb * b2.z,
        wa * a.w + wb * b2.w
    };
}

// ============================================================
// Transform (TRS decomposed)
// ============================================================

struct Transform {
    float3     translation = {};
    Quaternion rotation    = Quaternion::identity();
    float3     scale       = {1.0f, 1.0f, 1.0f};

    float4x4 to_matrix() const {
        float4x4 m = rotation.to_matrix();
        m.m[0][0] *= scale.x; m.m[0][1] *= scale.x; m.m[0][2] *= scale.x;
        m.m[1][0] *= scale.y; m.m[1][1] *= scale.y; m.m[1][2] *= scale.y;
        m.m[2][0] *= scale.z; m.m[2][1] *= scale.z; m.m[2][2] *= scale.z;
        m.m[3][0] = translation.x;
        m.m[3][1] = translation.y;
        m.m[3][2] = translation.z;
        return m;
    }

    static Transform identity() { return {}; }
};

/// Linear interpolation for Transform
inline Transform lerp_transform(const Transform& a, const Transform& b, float t) {
    Transform result;
    result.translation = {
        a.translation.x + t * (b.translation.x - a.translation.x),
        a.translation.y + t * (b.translation.y - a.translation.y),
        a.translation.z + t * (b.translation.z - a.translation.z)
    };
    result.rotation = slerp(a.rotation, b.rotation, t);
    result.scale = {
        a.scale.x + t * (b.scale.x - a.scale.x),
        a.scale.y + t * (b.scale.y - a.scale.y),
        a.scale.z + t * (b.scale.z - a.scale.z)
    };
    return result;
}

// ============================================================
// Interpolation Mode
// ============================================================

enum class InterpolationMode : uint8_t {
    STEP    = 0,  // No interpolation (snap to keyframe)
    LINEAR  = 1,  // Linear interpolation
    CUBIC   = 2   // Cubic Hermite / Bezier interpolation
};

// ============================================================
// Animation Wrap Mode
// ============================================================

enum class WrapMode : uint8_t {
    ONCE        = 0,  // Play once and stop
    LOOP        = 1,  // Loop from start
    PING_PONG   = 2,  // Reverse direction at endpoints
    CLAMP       = 3   // Hold last frame
};

// ============================================================
// Animation Channel Target
// ============================================================

enum class ChannelTarget : uint8_t {
    TRANSLATION = 0,
    ROTATION    = 1,
    SCALE       = 2,
    WEIGHTS     = 3   // Morph target weights
};

// ============================================================
// Keyframe
// ============================================================

struct Keyframe {
    float time = 0.0f;
    float value[4] = {};       // Up to 4 components (x, y, z, w)
    float in_tangent[4] = {};  // For cubic interpolation
    float out_tangent[4] = {}; // For cubic interpolation
};

// ============================================================
// Animation Channel — one property of one target over time
// ============================================================

struct AnimationChannel {
    uint32_t           target_index = 0;  // Bone index or object index
    ChannelTarget      target       = ChannelTarget::TRANSLATION;
    InterpolationMode  interpolation = InterpolationMode::LINEAR;
    std::vector<Keyframe> keyframes;
};

// ============================================================
// Animation Clip — a complete animation (walk, run, idle, etc.)
// ============================================================

struct AnimationClipDescriptor {
    std::string name;
    float       duration     = 0.0f;       // Total clip duration in seconds
    float       sample_rate  = 30.0f;      // Original sample rate (for reference)
    WrapMode    wrap_mode    = WrapMode::LOOP;
    std::vector<AnimationChannel> channels;
};

// ============================================================
// Skeleton / Bone Hierarchy
// ============================================================

struct Bone {
    std::string name;
    int32_t     parent_index = -1;   // -1 = root bone
    Transform   bind_pose;           // Local bind pose transform
    float4x4    inverse_bind_matrix; // Inverse bind matrix for skinning
};

struct SkeletonDescriptor {
    std::string        name;
    std::vector<Bone>  bones;
};

// ============================================================
// 2D Animation Types
// ============================================================

/// 2D Transform for sprite and UI animation
struct Transform2D {
    float x = 0.0f, y = 0.0f;           // Position
    float rotation = 0.0f;               // Rotation in radians
    float scale_x = 1.0f, scale_y = 1.0f; // Scale
    float anchor_x = 0.5f, anchor_y = 0.5f; // Anchor point (0-1)
    float opacity = 1.0f;

    static Transform2D identity() { return {}; }
};

/// 2D keyframe
struct Keyframe2D {
    float       time = 0.0f;
    Transform2D transform;
    InterpolationMode interpolation = InterpolationMode::LINEAR;
};

/// 2D animation channel targets
enum class Channel2DTarget : uint8_t {
    POSITION_X = 0,
    POSITION_Y = 1,
    ROTATION   = 2,
    SCALE_X    = 3,
    SCALE_Y    = 4,
    OPACITY    = 5,
    ANCHOR_X   = 6,
    ANCHOR_Y   = 7
};

/// 2D sprite frame for frame-by-frame animation
struct SpriteFrame {
    float    time     = 0.0f;
    uint32_t region_x = 0, region_y = 0;     // UV region on sprite sheet
    uint32_t region_w = 0, region_h = 0;
    TextureHandle texture = INVALID_TEXTURE;  // Optional per-frame texture override
};

// ============================================================
// Vector Animation Types
// ============================================================

/// SVG path command types
enum class PathCommandType : uint8_t {
    MOVE_TO     = 0,
    LINE_TO     = 1,
    QUAD_TO     = 2,
    CUBIC_TO    = 3,
    ARC_TO      = 4,
    CLOSE       = 5
};

/// A single path command with parameters
struct PathCommand {
    PathCommandType type = PathCommandType::MOVE_TO;
    float params[7] = {};  // Max params for arc_to (rx, ry, rotation, large, sweep, x, y)
};

/// Vector shape for animation
struct VectorPath {
    std::vector<PathCommand> commands;
    float fill_color[4]   = {1.0f, 1.0f, 1.0f, 1.0f};  // RGBA
    float stroke_color[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    float stroke_width    = 0.0f;
};

/// Vector animation keyframe (path morphing)
struct VectorKeyframe {
    float                   time = 0.0f;
    std::vector<VectorPath> paths;
    Transform2D             transform;
};

// ============================================================
// Rive Animation Types
// ============================================================

/// Rive state machine input types
enum class RiveInputType : uint8_t {
    BOOLEAN = 0,
    NUMBER  = 1,
    TRIGGER = 2
};

/// Rive state machine input descriptor
struct RiveInput {
    std::string   name;
    RiveInputType type = RiveInputType::BOOLEAN;
    float         number_value  = 0.0f;
    bool          boolean_value = false;
};

/// Rive artboard descriptor
struct RiveArtboardDescriptor {
    std::string               name;
    float                     width  = 0.0f;
    float                     height = 0.0f;
    std::vector<std::string>  animation_names;
    std::vector<std::string>  state_machine_names;
    std::vector<RiveInput>    inputs;
};

// ============================================================
// Lottie Animation Types
// ============================================================

/// Lottie composition descriptor
struct LottieCompositionDescriptor {
    std::string name;
    float       width       = 0.0f;
    float       height      = 0.0f;
    float       start_frame = 0.0f;
    float       end_frame   = 0.0f;
    float       frame_rate  = 30.0f;
    std::vector<std::string> layer_names;
    std::vector<std::string> marker_names;
};

/// Lottie marker for named time ranges
struct LottieMarker {
    std::string name;
    float       start_frame = 0.0f;
    float       end_frame   = 0.0f;
};

// ============================================================
// IK / FK Configuration
// ============================================================

/// IK solver type
enum class IKSolverType : uint8_t {
    CCD      = 0,  // Cyclic Coordinate Descent (fast, general)
    FABRIK   = 1,  // Forward And Backward Reaching IK (natural motion)
    TWO_BONE = 2   // Analytical two-bone solver (arm/leg)
};

/// IK chain descriptor
struct IKChainDescriptor {
    uint32_t     end_effector_bone  = 0;  // Bone index of the end effector
    uint32_t     chain_length       = 2;  // Number of bones in the chain
    IKSolverType solver_type        = IKSolverType::TWO_BONE;
    float3       target_position    = {};
    Quaternion   target_rotation    = Quaternion::identity();
    float        weight             = 1.0f;   // IK blend weight (0 = FK, 1 = full IK)
    uint32_t     max_iterations     = 10;
    float        tolerance          = 0.001f;
    bool         use_pole_vector    = false;
    float3       pole_vector        = {0.0f, 0.0f, 1.0f};
};

/// FK pose override for a specific bone
struct FKPoseOverride {
    uint32_t   bone_index = 0;
    Transform  local_transform;
    float      blend_weight = 1.0f;  // Blend with animation (0 = anim, 1 = FK override)
};

// ============================================================
// Animation Blend Mode
// ============================================================

enum class AnimBlendMode : uint8_t {
    OVERRIDE  = 0,  // Replace lower layers
    ADDITIVE  = 1,  // Add to lower layers
    MULTIPLY  = 2   // Multiply with lower layers
};

/// Animation playback state for a single clip
struct AnimationPlayback {
    AnimationClipHandle clip    = INVALID_ANIMATION_CLIP;
    float               time   = 0.0f;   // Current playback time
    float               speed  = 1.0f;   // Playback speed multiplier
    float               weight = 1.0f;   // Blend weight
    WrapMode            wrap   = WrapMode::LOOP;
    AnimBlendMode       blend  = AnimBlendMode::OVERRIDE;
    bool                playing = true;
};

// ============================================================
// Motion Distance Estimation
// ============================================================

/// Result of motion distance estimation
struct MotionDistanceResult {
    float total_distance     = 0.0f;  // Total root motion distance (world units)
    float average_speed      = 0.0f;  // Average speed (units/sec)
    float peak_speed         = 0.0f;  // Peak instantaneous speed
    float vertical_distance  = 0.0f;  // Total vertical displacement
    float horizontal_distance = 0.0f; // Total horizontal displacement (XZ plane)
    float3 net_displacement  = {};    // Start-to-end displacement vector
    std::vector<float> speed_curve;   // Speed samples over the clip duration
};

// ============================================================
// Animation Format Detection
// ============================================================

enum class AnimationFormat : uint8_t {
    UNKNOWN    = 0,
    FBX_BINARY = 1,
    FBX_ASCII  = 2,
    BVH        = 3,
    RIVE       = 4,
    LOTTIE     = 5,
    SVG_ANIM   = 6
};

} // namespace pictor
