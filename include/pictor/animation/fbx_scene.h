// FBX Scene -- Level 2+3 (Typed Objects + Connections)
//
// Interprets an FBXDocument (raw node tree) into typed objects:
//   Model / Geometry / Material / Texture / Video /
//   Deformer (Skin + Cluster) / BlendShape / Pose /
//   AnimStack / AnimLayer / AnimCurveNode / AnimCurve /
//   NodeAttribute / GlobalSettings
//
// Every object is keyed by its FBX UniqueId. The connection graph
// (Objects/Objects and Object/Property) is parsed and traversable.
//
// The upper FBXImporter (Level 4) projects these into Pictor runtime
// descriptors (Skeleton, AnimationClip, SkinMesh, ...).
#pragma once

#include "pictor/animation/fbx_document.h"
#include "pictor/core/types.h"

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace pictor {

/// FBX "UniqueId" — 64-bit stable object identifier used inside an FBX file.
using FBXObjectId = uint64_t;

/// Discriminator for the typed FBX object variants.
enum class FBXObjectType : uint8_t {
    UNKNOWN = 0,
    GLOBAL_SETTINGS,
    MODEL,                       ///< Transform node (Mesh / LimbNode / Null / Light / Camera)
    GEOMETRY,                    ///< Polygon mesh
    MATERIAL,
    TEXTURE,
    VIDEO,                       ///< Texture source (file path + optional embedded bytes)
    DEFORMER_SKIN,
    DEFORMER_CLUSTER,            ///< One bone within a skin
    DEFORMER_BLENDSHAPE,
    DEFORMER_BLENDSHAPE_CHANNEL,
    SHAPE_GEOMETRY,              ///< Per-channel morph target (vertex deltas)
    POSE,                        ///< BindPose: model -> matrix
    ANIMATION_STACK,
    ANIMATION_LAYER,
    ANIMATION_CURVE_NODE,        ///< Groups X/Y/Z curves bound to a Model property
    ANIMATION_CURVE,
    NODE_ATTRIBUTE,              ///< LimbNode / Light / Camera metadata attached to a Model
};

/// Rotation order as specified by FBX `RotationOrder` property.
/// Values match FBX SDK: 0 XYZ, 1 XZY, 2 YZX, 3 YXZ, 4 ZXY, 5 ZYX, 6 Spheric XYZ.
enum class FBXRotationOrder : uint8_t {
    XYZ = 0, XZY = 1, YZX = 2, YXZ = 3, ZXY = 4, ZYX = 5, SPHERIC_XYZ = 6,
};

// ── Connection graph ────────────────────────────────────────

struct FBXConnection {
    enum Kind : uint8_t { OO = 0, OP = 1 };
    Kind        kind = OO;
    FBXObjectId src  = 0;        ///< "source" — normally the child / owned object
    FBXObjectId dst  = 0;        ///< "destination" — normally the parent / owner
    std::string property;        ///< OP only: which Model property the source binds to
};

// ── Forward decl of the Object variant ─────────────────────

struct FBXObject;

// ── GlobalSettings ─────────────────────────────────────────

struct FBXGlobalSettings {
    int    up_axis        = 1;     ///< 0=X, 1=Y, 2=Z
    int    up_axis_sign   = 1;
    int    front_axis     = 2;
    int    front_axis_sign = 1;
    int    coord_axis     = 0;
    int    coord_axis_sign = 1;
    int    time_mode      = 6;     ///< 0 default, 6 = 24fps, etc.
    double unit_scale     = 1.0;   ///< cm per unit (FBX default 1.0 = cm)
    double original_unit_scale = 1.0;
    int64_t time_span_start = 0;   ///< KTime
    int64_t time_span_stop  = 0;
    double custom_frame_rate = 24.0;
};

// ── Model (transform node) ─────────────────────────────────

struct FBXModel {
    std::string          name;
    std::string          sub_type;      ///< "Mesh" / "LimbNode" / "Null" / "Light" / "Camera" / ...

    float3               translation{0, 0, 0};
    float3               rotation{0, 0, 0};        ///< Euler degrees
    float3               scaling{1, 1, 1};

    float3               rotation_offset{0, 0, 0};
    float3               rotation_pivot{0, 0, 0};
    float3               scaling_offset{0, 0, 0};
    float3               scaling_pivot{0, 0, 0};
    float3               pre_rotation{0, 0, 0};    ///< Euler degrees
    float3               post_rotation{0, 0, 0};   ///< Euler degrees

    FBXRotationOrder     rotation_order = FBXRotationOrder::XYZ;
    bool                 visibility = true;
    bool                 is_root    = false;       ///< No parent Model
};

// ── Geometry (polygon mesh) ────────────────────────────────

enum class FBXMappingMode : uint8_t {
    BY_POLYGON_VERTEX = 0,
    BY_VERTEX,
    BY_POLYGON,
    BY_EDGE,
    ALL_SAME,
    NONE,
};
enum class FBXReferenceMode : uint8_t {
    DIRECT = 0,
    INDEX_TO_DIRECT,
    INDEX,
};

template <typename T>
struct FBXLayerElement {
    std::string              name;
    FBXMappingMode           mapping   = FBXMappingMode::NONE;
    FBXReferenceMode         reference = FBXReferenceMode::DIRECT;
    std::vector<T>           data;          ///< Per the mapping / reference mode
    std::vector<int32_t>     index;         ///< When reference == INDEX_TO_DIRECT
};

struct FBXGeometry {
    std::vector<float3>      positions;
    /// FBX "PolygonVertexIndex": last index of each polygon is XOR'd with -1
    /// (bit-not). Values are indices into `positions`.
    std::vector<int32_t>     polygon_vertex_index;
    std::vector<int32_t>     edges;

    std::vector<FBXLayerElement<float3>> normals;
    std::vector<FBXLayerElement<float3>> tangents;
    std::vector<FBXLayerElement<float3>> binormals;
    std::vector<FBXLayerElement<float4>> colors;
    struct UVSet {
        std::string              name;
        FBXMappingMode           mapping   = FBXMappingMode::BY_POLYGON_VERTEX;
        FBXReferenceMode         reference = FBXReferenceMode::INDEX_TO_DIRECT;
        std::vector<float>       u;
        std::vector<float>       v;
        std::vector<int32_t>     index;
    };
    std::vector<UVSet>                     uv_sets;

    FBXLayerElement<int32_t>               material_indices;  ///< per-polygon
    FBXLayerElement<int32_t>               smoothing;

    /// Cached triangulated output (populated by FBXScene::triangulate).
    struct Triangulated {
        std::vector<float3>   positions;     ///< Per-tri-vertex, length = indices.size()
        std::vector<float3>   normals;
        std::vector<float4>   colors;
        std::vector<float3>   tangents;
        std::vector<float3>   binormals;
        /// First UV set, interleaved (u0,v0,u1,v1,...)
        std::vector<float>    uv0;
        std::vector<int32_t>  indices;       ///< 0..N trivial index array for draw calls
        std::vector<int32_t>  material_per_tri;
        /// For each tri-vertex: which position index it came from
        std::vector<int32_t>  original_vertex;
        /// For each tri: which polygon it was tessellated from
        std::vector<int32_t>  polygon_of_tri;
        bool                  valid = false;
    };
    mutable Triangulated      triangulated_cache;
};

// ── Skin / Cluster ─────────────────────────────────────────

struct FBXCluster {
    std::string              name;
    FBXObjectId              bone_model_id = 0;  ///< LimbNode Model this cluster is bound to
    std::vector<int32_t>     indices;            ///< vertex indices into Geometry.positions
    std::vector<double>      weights;
    float4x4                 transform           = float4x4::identity();  ///< Mesh -> cluster
    float4x4                 transform_link      = float4x4::identity();  ///< Bone -> world at bind time
    float4x4                 transform_associate = float4x4::identity();
};

struct FBXSkinDeformer {
    std::string              name;
    double                   skinning_type_blend_weight = 0.0;
    std::vector<FBXObjectId> cluster_ids;
};

// ── BlendShape / Morph ─────────────────────────────────────

struct FBXShapeGeometry {
    std::vector<int32_t>     indices;   ///< vertex indices the deltas apply to
    std::vector<float3>      position_deltas;
    std::vector<float3>      normal_deltas;   ///< may be empty
};

struct FBXBlendShapeChannel {
    std::string              name;
    double                   deform_percent = 0.0;
    std::vector<double>      full_weights;     ///< per-shape "FullWeights"
    std::vector<FBXObjectId> shape_ids;        ///< FBXShapeGeometry children
};

struct FBXBlendShape {
    std::string              name;
    std::vector<FBXObjectId> channel_ids;
};

// ── Material / Texture / Video ─────────────────────────────

struct FBXMaterial {
    std::string              name;
    std::string              shading_model;   ///< "Phong" / "Lambert" / ...

    float3                   ambient{0, 0, 0};
    float3                   diffuse{1, 1, 1};
    float3                   specular{0, 0, 0};
    float3                   emissive{0, 0, 0};
    float3                   reflection{0, 0, 0};
    float                    opacity       = 1.0f;
    float                    shininess     = 0.0f;
    float                    reflectivity  = 0.0f;
    float                    diffuse_factor  = 1.0f;
    float                    specular_factor = 1.0f;
    float                    emissive_factor = 1.0f;

    /// Channel name (e.g. "DiffuseColor", "NormalMap") -> FBXTexture id.
    std::unordered_map<std::string, FBXObjectId> textures;
};

struct FBXTexture {
    std::string              name;
    std::string              file_name;
    std::string              relative_filename;
    std::string              uv_set;
    int                      wrap_u = 0;   ///< 0 = repeat, 1 = clamp
    int                      wrap_v = 0;
    FBXObjectId              video_id = 0;
};

struct FBXVideo {
    std::string              name;
    std::string              file_name;
    std::string              relative_filename;
    std::vector<uint8_t>     content;   ///< Embedded image bytes (may be empty)
};

// ── BindPose ───────────────────────────────────────────────

struct FBXPose {
    std::string              name;
    bool                     is_bind_pose = false;
    std::unordered_map<FBXObjectId, float4x4> model_matrices;
};

// ── Animation ──────────────────────────────────────────────

struct FBXAnimCurve {
    /// KTime ticks (46186158000 per second). Convert via FBXScene::ktime_to_seconds().
    std::vector<int64_t>     key_time;
    std::vector<float>       key_value;
    std::vector<int32_t>     key_flag;
    std::vector<int32_t>     key_attr_flag;
    std::vector<float>       key_attr_data;
    std::vector<int32_t>     key_attr_ref_count;
};

struct FBXAnimCurveNode {
    std::string              name;
    /// "d|X", "d|Y", "d|Z", "X", "Y", "Z", "DeformPercent", ... -> curve id
    std::unordered_map<std::string, FBXObjectId> curves;
    /// Default values read from Properties70 (e.g. "d|X", "d|Y", "d|Z").
    std::unordered_map<std::string, double> default_values;
};

struct FBXAnimLayer {
    std::string              name;
    std::vector<FBXObjectId> curve_node_ids;
};

struct FBXAnimStack {
    std::string              name;
    int64_t                  local_start  = 0;
    int64_t                  local_stop   = 0;
    int64_t                  reference_start = 0;
    int64_t                  reference_stop  = 0;
    std::vector<FBXObjectId> layer_ids;
};

// ── NodeAttribute (LimbNode size, Light/Camera metadata) ───

struct FBXNodeAttribute {
    std::string              name;
    std::string              sub_type;      ///< "LimbNode" / "Light" / "Camera" / "Null"
    double                   limb_size = 1.0;
    /// Generic properties carried through for lights/cameras etc.
    std::unordered_map<std::string, double> numeric_props;
    std::unordered_map<std::string, std::string> string_props;
};

// ── Generic object wrapper ─────────────────────────────────

struct FBXObject {
    FBXObjectId       id   = 0;
    FBXObjectType     type = FBXObjectType::UNKNOWN;
    std::string       name;       ///< Display name (first Object property, ::-stripped)
    std::string       class_tag;  ///< FBX class tag ("Mesh" / "LimbNode" / "DiffuseColor" / ...)

    /// Back-reference to the raw source node (lifetime tied to FBXDocument).
    const FBXNode*    source_node = nullptr;

    // Typed payload (exactly one is non-null based on `type`).
    std::unique_ptr<FBXModel>             model;
    std::unique_ptr<FBXGeometry>          geometry;
    std::unique_ptr<FBXMaterial>          material;
    std::unique_ptr<FBXTexture>           texture;
    std::unique_ptr<FBXVideo>             video;
    std::unique_ptr<FBXSkinDeformer>      skin;
    std::unique_ptr<FBXCluster>           cluster;
    std::unique_ptr<FBXBlendShape>        blendshape;
    std::unique_ptr<FBXBlendShapeChannel> blendshape_channel;
    std::unique_ptr<FBXShapeGeometry>     shape_geometry;
    std::unique_ptr<FBXPose>              pose;
    std::unique_ptr<FBXAnimStack>         anim_stack;
    std::unique_ptr<FBXAnimLayer>         anim_layer;
    std::unique_ptr<FBXAnimCurveNode>     anim_curve_node;
    std::unique_ptr<FBXAnimCurve>         anim_curve;
    std::unique_ptr<FBXNodeAttribute>     node_attribute;
};

// ── FBXScene ───────────────────────────────────────────────

class FBXScene {
public:
    FBXScene() = default;
    ~FBXScene() = default;

    /// Parse an FBXDocument into typed objects + connection graph.
    /// Failure messages (non-fatal per-object issues are also appended)
    /// can be retrieved via `warnings()`.
    bool load(const FBXDocument& doc, std::string* error = nullptr);

    // -- Resource ID access (returns all owned data) --
    const FBXObject* get(FBXObjectId id) const noexcept;
    bool             has(FBXObjectId id) const noexcept;
    std::vector<FBXObjectId> all_ids() const;
    std::vector<FBXObjectId> ids_of_type(FBXObjectType type) const;

    // -- Connection traversal --

    /// Objects whose `dst == id` (= id's "children" in the FBX sense).
    std::vector<FBXObjectId> children_of(FBXObjectId id) const;
    /// Objects whose `src == id` (= id's "parents" in the FBX sense).
    std::vector<FBXObjectId> parents_of(FBXObjectId id) const;
    /// OP connections: src objects binding to `id`'s property `prop`.
    std::vector<FBXObjectId> connected_by_property(FBXObjectId id, const std::string& prop) const;

    /// Raw connection list (direct access).
    const std::vector<FBXConnection>& connections() const noexcept { return connections_; }

    // -- Utilities --

    /// Triangulate (fan) a Geometry and cache the result inside the object.
    /// Returns nullptr if id does not refer to a Geometry.
    const FBXGeometry::Triangulated* triangulate(FBXObjectId geometry_id) const;

    /// FBX local transform assembly following the SDK specification:
    /// T * Roff * Rp * Rpre * R * Rpost^-1 * Rp^-1 * Soff * Sp * S * Sp^-1
    float4x4 evaluate_local_transform(FBXObjectId model_id) const;
    /// Same as evaluate_local_transform but with T / R / S optionally
    /// substituted by animated samples (e.g. per-keyframe). Null pointers
    /// keep the model's static value.
    float4x4 evaluate_local_transform_with_anim(FBXObjectId model_id,
                                                const float3* anim_translation,
                                                const float3* anim_rotation_deg,
                                                const float3* anim_scaling) const;
    /// Accumulate from root Model. Returns identity if id is not a Model.
    float4x4 evaluate_world_transform(FBXObjectId model_id) const;

    /// KTime (FBX tick) -> seconds. 1 second == 46186158000 ticks.
    static double ktime_to_seconds(int64_t ktime) noexcept {
        return static_cast<double>(ktime) / 46186158000.0;
    }

    // -- Metadata --
    uint32_t               file_version = 0;
    std::string            creator;
    std::string            application_name;
    std::string            application_vendor;
    std::string            application_version;
    FBXGlobalSettings      global_settings;
    /// Model ids that have no Model parent (via OO connections).
    std::vector<FBXObjectId> root_model_ids;

    /// Non-fatal messages accumulated during load().
    const std::vector<std::string>& warnings() const noexcept { return warnings_; }

private:
    // Internal builders (implemented in fbx_scene.cpp).
    void parse_global_settings(const FBXNode& settings_node);
    void parse_documents(const FBXNode& docs_node);
    void parse_objects(const FBXNode& objects_node);
    void parse_connections(const FBXNode& connections_node);
    void finalize_root_models();
    FBXObject& ensure_object(FBXObjectId id);

    // Per-type parsers (return false only on fatal structural errors).
    void parse_model(FBXObject& obj, const FBXNode& node);
    void parse_geometry(FBXObject& obj, const FBXNode& node);
    void parse_material(FBXObject& obj, const FBXNode& node);
    void parse_texture(FBXObject& obj, const FBXNode& node);
    void parse_video(FBXObject& obj, const FBXNode& node);
    void parse_deformer(FBXObject& obj, const FBXNode& node);
    void parse_pose(FBXObject& obj, const FBXNode& node);
    void parse_anim_stack(FBXObject& obj, const FBXNode& node);
    void parse_anim_layer(FBXObject& obj, const FBXNode& node);
    void parse_anim_curve_node(FBXObject& obj, const FBXNode& node);
    void parse_anim_curve(FBXObject& obj, const FBXNode& node);
    void parse_node_attribute(FBXObject& obj, const FBXNode& node);

    void warn(std::string s) { warnings_.push_back(std::move(s)); }

private:
    std::unordered_map<FBXObjectId, std::unique_ptr<FBXObject>>     objects_;
    std::unordered_map<FBXObjectType, std::vector<FBXObjectId>>     ids_by_type_;
    std::vector<FBXConnection>                                       connections_;
    // Index for O(1) connection traversal.
    std::unordered_map<FBXObjectId, std::vector<size_t>>            conn_by_src_;
    std::unordered_map<FBXObjectId, std::vector<size_t>>            conn_by_dst_;
    std::vector<std::string>                                         warnings_;
};

} // namespace pictor
