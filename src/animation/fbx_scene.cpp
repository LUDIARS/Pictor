// FBX Scene -- Level 2+3 implementation
//
// Structure:
//   1. Helpers (Properties70 parser, LayerElement parser, math utils)
//   2. Per-type object parsers (Model / Geometry / Material / Texture /
//      Video / Deformer / BlendShape / Pose / Anim / NodeAttribute)
//   3. Connection graph builder
//   4. FBXScene::load orchestration
//   5. Accessors (get / has / ids / traversal)
//   6. Triangulation + transform evaluation
//
// No third-party dependencies. All parse failures are non-fatal and
// accumulate into warnings_; the scene loads in best-effort fashion.

#include "pictor/animation/fbx_scene.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <sstream>
#include <string>

namespace pictor {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kDegToRad = kPi / 180.0;

/// assign with explicit element cast to silence narrowing warnings.
template <typename Dst, typename Src>
void assign_cast(std::vector<Dst>& dst, const std::vector<Src>& src) {
    dst.clear();
    dst.reserve(src.size());
    for (const auto& v : src) dst.push_back(static_cast<Dst>(v));
}

inline float3 to_float3(const FBXProperty* p) {
    if (!p) return {};
    // Properties70 entries that represent vector values usually have 4 trailing
    // properties: "ColorType"|"Color"|"", "Color", "" and then r,g,b. We pull
    // the last three numeric values out of the tail.
    (void)p;
    return {};
}

// ─── Properties70 unit parser ──────────────────────────────
//
// Each "P" child of a Properties70 node has the shape:
//   P: "name", "type", "sub_type", "flags", ... values ...
// Examples:
//   P: "Lcl Translation", "Lcl Translation", "", "A", 1.0, 2.0, 3.0
//   P: "Visibility",      "Visibility",      "",  "A", 1
//   P: "RotationOrder",   "enum",            "",  "",  0
//   P: "FileName",        "KString",         "",  "", "path/to/file"

struct PropValue {
    std::string              str;
    std::vector<double>      numbers;
    bool                     has_string = false;
    bool                     has_numeric = false;
};

struct Properties70 {
    std::unordered_map<std::string, PropValue> map;

    const PropValue* get(const std::string& name) const {
        auto it = map.find(name);
        return (it == map.end()) ? nullptr : &it->second;
    }
    double number(const std::string& name, double fallback = 0.0) const {
        const PropValue* v = get(name);
        if (!v || v->numbers.empty()) return fallback;
        return v->numbers[0];
    }
    float3 vec3(const std::string& name, float3 fallback = {0, 0, 0}) const {
        const PropValue* v = get(name);
        if (!v || v->numbers.size() < 3) return fallback;
        return {static_cast<float>(v->numbers[0]),
                static_cast<float>(v->numbers[1]),
                static_cast<float>(v->numbers[2])};
    }
    std::string str(const std::string& name, const std::string& fallback = {}) const {
        const PropValue* v = get(name);
        if (!v || !v->has_string) return fallback;
        return v->str;
    }
    bool has(const std::string& name) const { return map.count(name) > 0; }
};

Properties70 parse_properties70(const FBXNode* node) {
    Properties70 out;
    if (!node) return out;
    for (const FBXNode& p : node->children) {
        if (p.name != "P" && p.name != "Property") continue;
        if (p.properties.empty()) continue;
        const std::string name = p.properties[0].as_string();
        PropValue v;
        // P: name, type, sub_type, flags, value0, value1, ...
        for (size_t i = 4; i < p.properties.size(); ++i) {
            const FBXProperty& pv = p.properties[i];
            if (pv.is_string()) {
                v.str = pv.as_string();
                v.has_string = true;
            } else {
                v.numbers.push_back(pv.as_double());
                v.has_numeric = true;
            }
        }
        if (v.numbers.empty() && !v.has_string) {
            // Fallback: older ASCII dumps might have the value at position 3
            if (p.properties.size() >= 4) {
                const FBXProperty& pv = p.properties[3];
                if (pv.is_string()) { v.str = pv.as_string(); v.has_string = true; }
                else { v.numbers.push_back(pv.as_double()); v.has_numeric = true; }
            }
        }
        out.map[name] = std::move(v);
    }
    return out;
}

// ─── Layer element parsing helpers ─────────────────────────

FBXMappingMode parse_mapping_mode(const std::string& s) {
    if (s == "ByPolygonVertex") return FBXMappingMode::BY_POLYGON_VERTEX;
    if (s == "ByVertex"   || s == "ByVertice") return FBXMappingMode::BY_VERTEX;
    if (s == "ByPolygon") return FBXMappingMode::BY_POLYGON;
    if (s == "ByEdge")    return FBXMappingMode::BY_EDGE;
    if (s == "AllSame")   return FBXMappingMode::ALL_SAME;
    return FBXMappingMode::NONE;
}
FBXReferenceMode parse_reference_mode(const std::string& s) {
    if (s == "IndexToDirect" || s == "Index To Direct") return FBXReferenceMode::INDEX_TO_DIRECT;
    if (s == "Index")  return FBXReferenceMode::INDEX;
    return FBXReferenceMode::DIRECT;
}

// Split a flat array of doubles (xyz triplets) into float3 list.
std::vector<float3> as_float3_list(const std::vector<double>& d) {
    std::vector<float3> r;
    r.reserve(d.size() / 3);
    for (size_t i = 0; i + 2 < d.size(); i += 3) {
        r.push_back({static_cast<float>(d[i]),
                     static_cast<float>(d[i + 1]),
                     static_cast<float>(d[i + 2])});
    }
    return r;
}
std::vector<float4> as_float4_list(const std::vector<double>& d) {
    std::vector<float4> r;
    r.reserve(d.size() / 4);
    for (size_t i = 0; i + 3 < d.size(); i += 4) {
        r.push_back({static_cast<float>(d[i]),
                     static_cast<float>(d[i + 1]),
                     static_cast<float>(d[i + 2]),
                     static_cast<float>(d[i + 3])});
    }
    return r;
}

// ─── Matrix helpers (row-major, row-vector convention) ─────
//
// Pictor's float4x4 stores m[row][col], with translation at m[3][0..2]
// and a transform of a row-vector is v * M.

float4x4 mat_identity() { return float4x4::identity(); }

float4x4 mat_from_row_major(const std::vector<double>& d) {
    float4x4 m{};
    if (d.size() >= 16) {
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c)
                m.m[r][c] = static_cast<float>(d[r * 4 + c]);
    } else {
        m = mat_identity();
    }
    return m;
}

float4x4 mat_mul(const float4x4& a, const float4x4& b) {
    float4x4 r{};
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) {
            float s = 0.0f;
            for (int k = 0; k < 4; ++k) s += a.m[i][k] * b.m[k][j];
            r.m[i][j] = s;
        }
    }
    return r;
}

float4x4 mat_translation(const float3& t) {
    float4x4 m = mat_identity();
    m.m[3][0] = t.x; m.m[3][1] = t.y; m.m[3][2] = t.z;
    return m;
}

float4x4 mat_scale(const float3& s) {
    float4x4 m{};
    m.m[0][0] = s.x; m.m[1][1] = s.y; m.m[2][2] = s.z; m.m[3][3] = 1.0f;
    return m;
}

float4x4 mat_rotation_euler(const float3& deg, FBXRotationOrder order) {
    const float rx = static_cast<float>(deg.x * kDegToRad);
    const float ry = static_cast<float>(deg.y * kDegToRad);
    const float rz = static_cast<float>(deg.z * kDegToRad);
    auto Rx = [&]() {
        float c = std::cos(rx), s = std::sin(rx);
        float4x4 m = mat_identity();
        m.m[1][1] =  c; m.m[1][2] = s;
        m.m[2][1] = -s; m.m[2][2] = c;
        return m;
    };
    auto Ry = [&]() {
        float c = std::cos(ry), s = std::sin(ry);
        float4x4 m = mat_identity();
        m.m[0][0] = c; m.m[0][2] = -s;
        m.m[2][0] = s; m.m[2][2] =  c;
        return m;
    };
    auto Rz = [&]() {
        float c = std::cos(rz), s = std::sin(rz);
        float4x4 m = mat_identity();
        m.m[0][0] =  c; m.m[0][1] = s;
        m.m[1][0] = -s; m.m[1][1] = c;
        return m;
    };
    // With row-vector convention, composing Ra * Rb means first applying Ra
    // then Rb. FBX rotation order XYZ = v' = v * Rx * Ry * Rz.
    switch (order) {
        case FBXRotationOrder::XYZ: return mat_mul(mat_mul(Rx(), Ry()), Rz());
        case FBXRotationOrder::XZY: return mat_mul(mat_mul(Rx(), Rz()), Ry());
        case FBXRotationOrder::YZX: return mat_mul(mat_mul(Ry(), Rz()), Rx());
        case FBXRotationOrder::YXZ: return mat_mul(mat_mul(Ry(), Rx()), Rz());
        case FBXRotationOrder::ZXY: return mat_mul(mat_mul(Rz(), Rx()), Ry());
        case FBXRotationOrder::ZYX: return mat_mul(mat_mul(Rz(), Ry()), Rx());
        default:                    return mat_mul(mat_mul(Rx(), Ry()), Rz());
    }
}

float4x4 mat_inverse_rigid(const float4x4& m) {
    // Inverse assuming the matrix is a rigid transform (orthonormal rotation +
    // translation, scale == 1). Rotation inverse = transpose. Translation
    // inverse = -R^T * t.
    float4x4 r = mat_identity();
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            r.m[i][j] = m.m[j][i];
    float tx = m.m[3][0], ty = m.m[3][1], tz = m.m[3][2];
    r.m[3][0] = -(r.m[0][0] * tx + r.m[1][0] * ty + r.m[2][0] * tz);
    r.m[3][1] = -(r.m[0][1] * tx + r.m[1][1] * ty + r.m[2][1] * tz);
    r.m[3][2] = -(r.m[0][2] * tx + r.m[1][2] * ty + r.m[2][2] * tz);
    return r;
}

// ─── Object id / name helpers ─────────────────────────────

FBXObjectId read_object_id(const FBXNode& node) {
    if (node.properties.empty()) return 0;
    return static_cast<FBXObjectId>(node.properties[0].as_int());
}

// FBX stores object "name" as "RealName\x00\x01SubClass" in binary, or as
// "SubClass::RealName" in ASCII. Extract the real name.
std::string extract_object_name(const std::string& raw) {
    auto sep = raw.find("\x00\x01", 0, 2);
    if (sep != std::string::npos) return raw.substr(0, sep);
    auto sc = raw.find("::");
    if (sc != std::string::npos) return raw.substr(sc + 2);
    return raw;
}
std::string extract_object_class(const std::string& raw) {
    auto sep = raw.find("\x00\x01", 0, 2);
    if (sep != std::string::npos) return raw.substr(sep + 2);
    auto sc = raw.find("::");
    if (sc != std::string::npos) return raw.substr(0, sc);
    return {};
}

// Derive FBXObjectType from the node name + class tag. Returns UNKNOWN if not
// recognized.
FBXObjectType classify(const std::string& node_name,
                        const std::string& class_tag) {
    if (node_name == "Model") return FBXObjectType::MODEL;
    if (node_name == "Geometry") {
        if (class_tag == "Shape") return FBXObjectType::SHAPE_GEOMETRY;
        return FBXObjectType::GEOMETRY;
    }
    if (node_name == "Material")       return FBXObjectType::MATERIAL;
    if (node_name == "Texture")        return FBXObjectType::TEXTURE;
    if (node_name == "Video")          return FBXObjectType::VIDEO;
    if (node_name == "Deformer") {
        if (class_tag == "Skin")             return FBXObjectType::DEFORMER_SKIN;
        if (class_tag == "Cluster")          return FBXObjectType::DEFORMER_CLUSTER;
        if (class_tag == "BlendShape")       return FBXObjectType::DEFORMER_BLENDSHAPE;
        if (class_tag == "BlendShapeChannel") return FBXObjectType::DEFORMER_BLENDSHAPE_CHANNEL;
        return FBXObjectType::UNKNOWN;
    }
    if (node_name == "Pose")          return FBXObjectType::POSE;
    if (node_name == "AnimationStack") return FBXObjectType::ANIMATION_STACK;
    if (node_name == "AnimationLayer") return FBXObjectType::ANIMATION_LAYER;
    if (node_name == "AnimationCurveNode") return FBXObjectType::ANIMATION_CURVE_NODE;
    if (node_name == "AnimationCurve")     return FBXObjectType::ANIMATION_CURVE;
    if (node_name == "NodeAttribute")      return FBXObjectType::NODE_ATTRIBUTE;
    return FBXObjectType::UNKNOWN;
}

FBXRotationOrder int_to_rotation_order(int v) {
    switch (v) {
        case 0: return FBXRotationOrder::XYZ;
        case 1: return FBXRotationOrder::XZY;
        case 2: return FBXRotationOrder::YZX;
        case 3: return FBXRotationOrder::YXZ;
        case 4: return FBXRotationOrder::ZXY;
        case 5: return FBXRotationOrder::ZYX;
        case 6: return FBXRotationOrder::SPHERIC_XYZ;
        default: return FBXRotationOrder::XYZ;
    }
}

} // namespace

// ─── Per-type parsers ──────────────────────────────────────

void FBXScene::parse_model(FBXObject& obj, const FBXNode& node) {
    obj.model = std::make_unique<FBXModel>();
    FBXModel& m = *obj.model;
    m.name = obj.name;
    if (node.properties.size() >= 3) {
        m.sub_type = node.properties[2].as_string();
    }
    const FBXNode* props_node = node.find_child("Properties70");
    if (!props_node) props_node = node.find_child("Properties60");
    Properties70 p = parse_properties70(props_node);

    m.translation   = p.vec3("Lcl Translation",  m.translation);
    m.rotation      = p.vec3("Lcl Rotation",     m.rotation);
    m.scaling       = p.vec3("Lcl Scaling",      m.scaling);
    m.rotation_offset = p.vec3("RotationOffset", m.rotation_offset);
    m.rotation_pivot  = p.vec3("RotationPivot",  m.rotation_pivot);
    m.scaling_offset  = p.vec3("ScalingOffset",  m.scaling_offset);
    m.scaling_pivot   = p.vec3("ScalingPivot",   m.scaling_pivot);
    m.pre_rotation    = p.vec3("PreRotation",    m.pre_rotation);
    m.post_rotation   = p.vec3("PostRotation",   m.post_rotation);

    m.rotation_order = int_to_rotation_order(static_cast<int>(p.number("RotationOrder", 0)));
    m.visibility     = (p.number("Visibility", 1) != 0);
}

void FBXScene::parse_geometry(FBXObject& obj, const FBXNode& node) {
    obj.geometry = std::make_unique<FBXGeometry>();
    FBXGeometry& g = *obj.geometry;

    if (const FBXNode* v = node.find_child("Vertices")) {
        if (!v->properties.empty()) {
            g.positions = as_float3_list(v->properties[0].as_double_array());
        }
    }
    if (const FBXNode* v = node.find_child("PolygonVertexIndex")) {
        if (!v->properties.empty()) {
            assign_cast(g.polygon_vertex_index, v->properties[0].as_int_array());
        }
    }
    if (const FBXNode* v = node.find_child("Edges")) {
        if (!v->properties.empty()) {
            assign_cast(g.edges, v->properties[0].as_int_array());
        }
    }

    // LayerElementNormal / Tangent / Binormal / UV / Color / Material / Smoothing
    for (const FBXNode& child : node.children) {
        if (child.name == "LayerElementNormal") {
            FBXLayerElement<float3> le;
            if (const FBXNode* n = child.find_child("Name")) if (!n->properties.empty()) le.name = n->properties[0].as_string();
            if (const FBXNode* n = child.find_child("MappingInformationType"))   le.mapping   = parse_mapping_mode(n->properties.empty() ? "" : n->properties[0].as_string());
            if (const FBXNode* n = child.find_child("ReferenceInformationType")) le.reference = parse_reference_mode(n->properties.empty() ? "" : n->properties[0].as_string());
            if (const FBXNode* n = child.find_child("Normals")) if (!n->properties.empty()) le.data = as_float3_list(n->properties[0].as_double_array());
            if (const FBXNode* n = child.find_child("NormalsIndex")) if (!n->properties.empty()) {
                assign_cast(le.index, n->properties[0].as_int_array());
            }
            g.normals.push_back(std::move(le));
        } else if (child.name == "LayerElementTangent") {
            FBXLayerElement<float3> le;
            if (const FBXNode* n = child.find_child("MappingInformationType"))   le.mapping   = parse_mapping_mode(n->properties.empty() ? "" : n->properties[0].as_string());
            if (const FBXNode* n = child.find_child("ReferenceInformationType")) le.reference = parse_reference_mode(n->properties.empty() ? "" : n->properties[0].as_string());
            if (const FBXNode* n = child.find_child("Tangents")) if (!n->properties.empty()) le.data = as_float3_list(n->properties[0].as_double_array());
            g.tangents.push_back(std::move(le));
        } else if (child.name == "LayerElementBinormal") {
            FBXLayerElement<float3> le;
            if (const FBXNode* n = child.find_child("MappingInformationType"))   le.mapping   = parse_mapping_mode(n->properties.empty() ? "" : n->properties[0].as_string());
            if (const FBXNode* n = child.find_child("ReferenceInformationType")) le.reference = parse_reference_mode(n->properties.empty() ? "" : n->properties[0].as_string());
            if (const FBXNode* n = child.find_child("Binormals")) if (!n->properties.empty()) le.data = as_float3_list(n->properties[0].as_double_array());
            g.binormals.push_back(std::move(le));
        } else if (child.name == "LayerElementColor") {
            FBXLayerElement<float4> le;
            if (const FBXNode* n = child.find_child("MappingInformationType"))   le.mapping   = parse_mapping_mode(n->properties.empty() ? "" : n->properties[0].as_string());
            if (const FBXNode* n = child.find_child("ReferenceInformationType")) le.reference = parse_reference_mode(n->properties.empty() ? "" : n->properties[0].as_string());
            if (const FBXNode* n = child.find_child("Colors")) if (!n->properties.empty()) le.data = as_float4_list(n->properties[0].as_double_array());
            if (const FBXNode* n = child.find_child("ColorIndex")) if (!n->properties.empty()) {
                assign_cast(le.index, n->properties[0].as_int_array());
            }
            g.colors.push_back(std::move(le));
        } else if (child.name == "LayerElementUV") {
            FBXGeometry::UVSet uv;
            if (const FBXNode* n = child.find_child("Name")) if (!n->properties.empty()) uv.name = n->properties[0].as_string();
            if (const FBXNode* n = child.find_child("MappingInformationType"))   uv.mapping   = parse_mapping_mode(n->properties.empty() ? "" : n->properties[0].as_string());
            if (const FBXNode* n = child.find_child("ReferenceInformationType")) uv.reference = parse_reference_mode(n->properties.empty() ? "" : n->properties[0].as_string());
            if (const FBXNode* n = child.find_child("UV")) if (!n->properties.empty()) {
                auto d = n->properties[0].as_double_array();
                uv.u.reserve(d.size() / 2);
                uv.v.reserve(d.size() / 2);
                for (size_t i = 0; i + 1 < d.size(); i += 2) {
                    uv.u.push_back(static_cast<float>(d[i]));
                    uv.v.push_back(static_cast<float>(d[i + 1]));
                }
            }
            if (const FBXNode* n = child.find_child("UVIndex")) if (!n->properties.empty()) {
                assign_cast(uv.index, n->properties[0].as_int_array());
            }
            g.uv_sets.push_back(std::move(uv));
        } else if (child.name == "LayerElementMaterial") {
            if (const FBXNode* n = child.find_child("MappingInformationType"))   g.material_indices.mapping   = parse_mapping_mode(n->properties.empty() ? "" : n->properties[0].as_string());
            if (const FBXNode* n = child.find_child("ReferenceInformationType")) g.material_indices.reference = parse_reference_mode(n->properties.empty() ? "" : n->properties[0].as_string());
            if (const FBXNode* n = child.find_child("Materials")) if (!n->properties.empty()) {
                assign_cast(g.material_indices.data, n->properties[0].as_int_array());
            }
        } else if (child.name == "LayerElementSmoothing") {
            if (const FBXNode* n = child.find_child("MappingInformationType"))   g.smoothing.mapping   = parse_mapping_mode(n->properties.empty() ? "" : n->properties[0].as_string());
            if (const FBXNode* n = child.find_child("ReferenceInformationType")) g.smoothing.reference = parse_reference_mode(n->properties.empty() ? "" : n->properties[0].as_string());
            if (const FBXNode* n = child.find_child("Smoothing")) if (!n->properties.empty()) {
                assign_cast(g.smoothing.data, n->properties[0].as_int_array());
            }
        }
    }
}

void FBXScene::parse_material(FBXObject& obj, const FBXNode& node) {
    obj.material = std::make_unique<FBXMaterial>();
    FBXMaterial& m = *obj.material;
    m.name = obj.name;
    if (const FBXNode* sm = node.find_child("ShadingModel")) {
        if (!sm->properties.empty()) m.shading_model = sm->properties[0].as_string();
    }
    Properties70 p = parse_properties70(node.find_child("Properties70"));
    m.ambient   = p.vec3("AmbientColor",  m.ambient);
    m.diffuse   = p.vec3("DiffuseColor",  m.diffuse);
    m.specular  = p.vec3("SpecularColor", m.specular);
    m.emissive  = p.vec3("EmissiveColor", m.emissive);
    m.reflection = p.vec3("ReflectionColor", m.reflection);
    m.opacity        = static_cast<float>(p.number("Opacity",       m.opacity));
    m.shininess      = static_cast<float>(p.number("Shininess",     m.shininess));
    m.shininess      = static_cast<float>(p.number("ShininessExponent", m.shininess));
    m.reflectivity   = static_cast<float>(p.number("Reflectivity",  m.reflectivity));
    m.diffuse_factor  = static_cast<float>(p.number("DiffuseFactor",  m.diffuse_factor));
    m.specular_factor = static_cast<float>(p.number("SpecularFactor", m.specular_factor));
    m.emissive_factor = static_cast<float>(p.number("EmissiveFactor", m.emissive_factor));
}

void FBXScene::parse_texture(FBXObject& obj, const FBXNode& node) {
    obj.texture = std::make_unique<FBXTexture>();
    FBXTexture& t = *obj.texture;
    t.name = obj.name;
    if (const FBXNode* n = node.find_child("FileName"))         if (!n->properties.empty()) t.file_name = n->properties[0].as_string();
    if (const FBXNode* n = node.find_child("RelativeFilename")) if (!n->properties.empty()) t.relative_filename = n->properties[0].as_string();
    Properties70 p = parse_properties70(node.find_child("Properties70"));
    if (p.has("UVSet"))      t.uv_set = p.str("UVSet");
    if (p.has("WrapModeU"))  t.wrap_u = static_cast<int>(p.number("WrapModeU", 0));
    if (p.has("WrapModeV"))  t.wrap_v = static_cast<int>(p.number("WrapModeV", 0));
}

void FBXScene::parse_video(FBXObject& obj, const FBXNode& node) {
    obj.video = std::make_unique<FBXVideo>();
    FBXVideo& v = *obj.video;
    v.name = obj.name;
    if (const FBXNode* n = node.find_child("FileName"))         if (!n->properties.empty()) v.file_name = n->properties[0].as_string();
    if (const FBXNode* n = node.find_child("RelativeFilename")) if (!n->properties.empty()) v.relative_filename = n->properties[0].as_string();
    if (const FBXNode* n = node.find_child("Content")) {
        if (!n->properties.empty()) {
            const FBXProperty& pp = n->properties[0];
            if (pp.type == FBXPropertyType::RAW || pp.type == FBXPropertyType::STRING) {
                v.content.assign(pp.str.begin(), pp.str.end());
            }
        }
    }
}

void FBXScene::parse_deformer(FBXObject& obj, const FBXNode& node) {
    // The Deformer node may represent Skin, Cluster, BlendShape, or
    // BlendShapeChannel. The class tag determined the object type earlier.
    switch (obj.type) {
        case FBXObjectType::DEFORMER_SKIN: {
            obj.skin = std::make_unique<FBXSkinDeformer>();
            FBXSkinDeformer& s = *obj.skin;
            s.name = obj.name;
            if (const FBXNode* n = node.find_child("SkinningType")) {
                // "Linear", "Blend" etc. -- parsed loosely.
                (void)n;
            }
            Properties70 p = parse_properties70(node.find_child("Properties70"));
            s.skinning_type_blend_weight = p.number("BlendWeights", 0.0);
            break;
        }
        case FBXObjectType::DEFORMER_CLUSTER: {
            obj.cluster = std::make_unique<FBXCluster>();
            FBXCluster& c = *obj.cluster;
            c.name = obj.name;
            if (const FBXNode* n = node.find_child("Indexes")) {
                if (!n->properties.empty()) {
                    assign_cast(c.indices, n->properties[0].as_int_array());
                }
            }
            if (const FBXNode* n = node.find_child("Weights")) {
                if (!n->properties.empty()) c.weights = n->properties[0].as_double_array();
            }
            if (const FBXNode* n = node.find_child("Transform")) {
                if (!n->properties.empty()) c.transform = mat_from_row_major(n->properties[0].as_double_array());
            }
            if (const FBXNode* n = node.find_child("TransformLink")) {
                if (!n->properties.empty()) c.transform_link = mat_from_row_major(n->properties[0].as_double_array());
            }
            if (const FBXNode* n = node.find_child("TransformAssociateModel")) {
                if (!n->properties.empty()) c.transform_associate = mat_from_row_major(n->properties[0].as_double_array());
            }
            break;
        }
        case FBXObjectType::DEFORMER_BLENDSHAPE: {
            obj.blendshape = std::make_unique<FBXBlendShape>();
            obj.blendshape->name = obj.name;
            break;
        }
        case FBXObjectType::DEFORMER_BLENDSHAPE_CHANNEL: {
            obj.blendshape_channel = std::make_unique<FBXBlendShapeChannel>();
            FBXBlendShapeChannel& c = *obj.blendshape_channel;
            c.name = obj.name;
            Properties70 p = parse_properties70(node.find_child("Properties70"));
            c.deform_percent = p.number("DeformPercent", 0.0);
            if (const FBXNode* n = node.find_child("FullWeights")) {
                if (!n->properties.empty()) c.full_weights = n->properties[0].as_double_array();
            }
            break;
        }
        default: break;
    }
}

void FBXScene::parse_pose(FBXObject& obj, const FBXNode& node) {
    obj.pose = std::make_unique<FBXPose>();
    FBXPose& p = *obj.pose;
    p.name = obj.name;
    if (node.properties.size() >= 3) {
        p.is_bind_pose = (node.properties[2].as_string() == "BindPose");
    }
    for (const FBXNode& child : node.children) {
        if (child.name != "PoseNode") continue;
        FBXObjectId target_id = 0;
        float4x4    matrix    = mat_identity();
        if (const FBXNode* n = child.find_child("Node")) {
            if (!n->properties.empty()) target_id = static_cast<FBXObjectId>(n->properties[0].as_int());
        }
        if (const FBXNode* n = child.find_child("Matrix")) {
            if (!n->properties.empty()) matrix = mat_from_row_major(n->properties[0].as_double_array());
        }
        if (target_id != 0) p.model_matrices[target_id] = matrix;
    }
}

void FBXScene::parse_anim_stack(FBXObject& obj, const FBXNode& node) {
    obj.anim_stack = std::make_unique<FBXAnimStack>();
    FBXAnimStack& s = *obj.anim_stack;
    s.name = obj.name;
    Properties70 p = parse_properties70(node.find_child("Properties70"));
    s.local_start     = static_cast<int64_t>(p.number("LocalStart",     0));
    s.local_stop      = static_cast<int64_t>(p.number("LocalStop",      0));
    s.reference_start = static_cast<int64_t>(p.number("ReferenceStart", 0));
    s.reference_stop  = static_cast<int64_t>(p.number("ReferenceStop",  0));
}

void FBXScene::parse_anim_layer(FBXObject& obj, const FBXNode& /*node*/) {
    obj.anim_layer = std::make_unique<FBXAnimLayer>();
    obj.anim_layer->name = obj.name;
}

void FBXScene::parse_anim_curve_node(FBXObject& obj, const FBXNode& node) {
    obj.anim_curve_node = std::make_unique<FBXAnimCurveNode>();
    obj.anim_curve_node->name = obj.name;
    // Default values are stored in Properties70 under names like "d|X".
    Properties70 p = parse_properties70(node.find_child("Properties70"));
    for (const auto& kv : p.map) {
        if (!kv.second.numbers.empty() && kv.first.rfind("d|", 0) == 0) {
            obj.anim_curve_node->default_values[kv.first] = kv.second.numbers[0];
        }
    }
}

void FBXScene::parse_anim_curve(FBXObject& obj, const FBXNode& node) {
    obj.anim_curve = std::make_unique<FBXAnimCurve>();
    FBXAnimCurve& c = *obj.anim_curve;
    if (const FBXNode* n = node.find_child("KeyTime")) {
        if (!n->properties.empty()) c.key_time = n->properties[0].as_int_array();
    }
    if (const FBXNode* n = node.find_child("KeyValueFloat")) {
        if (!n->properties.empty()) assign_cast(c.key_value, n->properties[0].as_double_array());
    }
    if (const FBXNode* n = node.find_child("KeyAttrFlags")) {
        if (!n->properties.empty()) assign_cast(c.key_flag, n->properties[0].as_int_array());
    }
    if (const FBXNode* n = node.find_child("KeyAttrDataFloat")) {
        if (!n->properties.empty()) assign_cast(c.key_attr_data, n->properties[0].as_double_array());
    }
    if (const FBXNode* n = node.find_child("KeyAttrRefCount")) {
        if (!n->properties.empty()) assign_cast(c.key_attr_ref_count, n->properties[0].as_int_array());
    }
}

void FBXScene::parse_node_attribute(FBXObject& obj, const FBXNode& node) {
    obj.node_attribute = std::make_unique<FBXNodeAttribute>();
    FBXNodeAttribute& a = *obj.node_attribute;
    a.name = obj.name;
    a.sub_type = obj.class_tag;
    Properties70 p = parse_properties70(node.find_child("Properties70"));
    a.limb_size = p.number("Size", 1.0);
    for (const auto& kv : p.map) {
        if (kv.second.has_string) a.string_props[kv.first] = kv.second.str;
        else if (!kv.second.numbers.empty()) a.numeric_props[kv.first] = kv.second.numbers[0];
    }
}

// ─── Top-level load ────────────────────────────────────────

FBXObject& FBXScene::ensure_object(FBXObjectId id) {
    auto it = objects_.find(id);
    if (it != objects_.end()) return *it->second;
    auto uptr = std::make_unique<FBXObject>();
    uptr->id = id;
    FBXObject* raw = uptr.get();
    objects_.emplace(id, std::move(uptr));
    return *raw;
}

void FBXScene::parse_global_settings(const FBXNode& settings_node) {
    Properties70 p = parse_properties70(settings_node.find_child("Properties70"));
    global_settings.up_axis         = static_cast<int>(p.number("UpAxis",         1));
    global_settings.up_axis_sign    = static_cast<int>(p.number("UpAxisSign",     1));
    global_settings.front_axis      = static_cast<int>(p.number("FrontAxis",      2));
    global_settings.front_axis_sign = static_cast<int>(p.number("FrontAxisSign",  1));
    global_settings.coord_axis      = static_cast<int>(p.number("CoordAxis",      0));
    global_settings.coord_axis_sign = static_cast<int>(p.number("CoordAxisSign",  1));
    global_settings.time_mode       = static_cast<int>(p.number("TimeMode",       6));
    global_settings.unit_scale      = p.number("UnitScaleFactor",         1.0);
    global_settings.original_unit_scale = p.number("OriginalUnitScaleFactor", 1.0);
    global_settings.time_span_start = static_cast<int64_t>(p.number("TimeSpanStart", 0));
    global_settings.time_span_stop  = static_cast<int64_t>(p.number("TimeSpanStop",  0));
    global_settings.custom_frame_rate = p.number("CustomFrameRate", 24.0);
}

void FBXScene::parse_documents(const FBXNode& docs_node) {
    // Not strictly needed for single-document FBX, but we capture Creator /
    // ApplicationName for metadata.
    for (const FBXNode& doc : docs_node.children) {
        if (doc.name != "Document") continue;
        if (const FBXNode* n = doc.find_child("RootNode")) (void)n;
    }
}

void FBXScene::parse_objects(const FBXNode& objects_node) {
    for (const FBXNode& child : objects_node.children) {
        FBXObjectId id = read_object_id(child);
        if (id == 0) continue;
        std::string raw_name = (child.properties.size() >= 2) ? child.properties[1].as_string() : std::string{};
        std::string class_tag = (child.properties.size() >= 3) ? child.properties[2].as_string() : std::string{};
        if (class_tag.empty()) {
            class_tag = extract_object_class(raw_name);
        }
        std::string name = extract_object_name(raw_name);

        FBXObjectType type = classify(child.name, class_tag);
        FBXObject& obj = ensure_object(id);
        obj.type        = type;
        obj.name        = name;
        obj.class_tag   = class_tag;
        obj.source_node = &child;

        switch (type) {
            case FBXObjectType::MODEL:                  parse_model(obj, child); break;
            case FBXObjectType::GEOMETRY:               parse_geometry(obj, child); break;
            case FBXObjectType::SHAPE_GEOMETRY: {
                obj.shape_geometry = std::make_unique<FBXShapeGeometry>();
                FBXShapeGeometry& sg = *obj.shape_geometry;
                if (const FBXNode* n = child.find_child("Indexes")) {
                    if (!n->properties.empty()) assign_cast(sg.indices, n->properties[0].as_int_array());
                }
                if (const FBXNode* n = child.find_child("Vertices")) {
                    if (!n->properties.empty()) sg.position_deltas = as_float3_list(n->properties[0].as_double_array());
                }
                if (const FBXNode* n = child.find_child("Normals")) {
                    if (!n->properties.empty()) sg.normal_deltas = as_float3_list(n->properties[0].as_double_array());
                }
                break;
            }
            case FBXObjectType::MATERIAL: parse_material(obj, child); break;
            case FBXObjectType::TEXTURE:  parse_texture(obj, child);  break;
            case FBXObjectType::VIDEO:    parse_video(obj, child);    break;
            case FBXObjectType::DEFORMER_SKIN:
            case FBXObjectType::DEFORMER_CLUSTER:
            case FBXObjectType::DEFORMER_BLENDSHAPE:
            case FBXObjectType::DEFORMER_BLENDSHAPE_CHANNEL:
                parse_deformer(obj, child); break;
            case FBXObjectType::POSE:                 parse_pose(obj, child); break;
            case FBXObjectType::ANIMATION_STACK:      parse_anim_stack(obj, child); break;
            case FBXObjectType::ANIMATION_LAYER:      parse_anim_layer(obj, child); break;
            case FBXObjectType::ANIMATION_CURVE_NODE: parse_anim_curve_node(obj, child); break;
            case FBXObjectType::ANIMATION_CURVE:      parse_anim_curve(obj, child); break;
            case FBXObjectType::NODE_ATTRIBUTE:       parse_node_attribute(obj, child); break;
            default: break;
        }
        ids_by_type_[type].push_back(id);
    }
}

void FBXScene::parse_connections(const FBXNode& connections_node) {
    for (const FBXNode& child : connections_node.children) {
        if (child.name != "C" && child.name != "Connect") continue;
        if (child.properties.size() < 3) continue;
        const std::string kind = child.properties[0].as_string();
        FBXObjectId src = static_cast<FBXObjectId>(child.properties[1].as_int());
        FBXObjectId dst = static_cast<FBXObjectId>(child.properties[2].as_int());
        FBXConnection c;
        c.src = src;
        c.dst = dst;
        if (kind == "OP") {
            c.kind = FBXConnection::OP;
            if (child.properties.size() >= 4) c.property = child.properties[3].as_string();
        } else {
            c.kind = FBXConnection::OO;
        }
        size_t idx = connections_.size();
        connections_.push_back(std::move(c));
        conn_by_src_[src].push_back(idx);
        conn_by_dst_[dst].push_back(idx);
    }
}

void FBXScene::finalize_root_models() {
    // Post-process connections to fill aggregate relationships that need
    // indices from both sides.
    for (const FBXConnection& c : connections_) {
        const FBXObject* src = get(c.src);
        const FBXObject* dst = get(c.dst);
        if (!src || !dst) continue;

        if (c.kind == FBXConnection::OO) {
            // Skin -> Cluster
            if (src->type == FBXObjectType::DEFORMER_CLUSTER && dst->type == FBXObjectType::DEFORMER_SKIN) {
                if (dst->skin) dst->skin->cluster_ids.push_back(src->id);
            }
            // BlendShape -> Channel
            if (src->type == FBXObjectType::DEFORMER_BLENDSHAPE_CHANNEL && dst->type == FBXObjectType::DEFORMER_BLENDSHAPE) {
                if (dst->blendshape) dst->blendshape->channel_ids.push_back(src->id);
            }
            // Channel -> Shape
            if (src->type == FBXObjectType::SHAPE_GEOMETRY && dst->type == FBXObjectType::DEFORMER_BLENDSHAPE_CHANNEL) {
                if (dst->blendshape_channel) dst->blendshape_channel->shape_ids.push_back(src->id);
            }
            // AnimLayer -> AnimStack
            if (src->type == FBXObjectType::ANIMATION_LAYER && dst->type == FBXObjectType::ANIMATION_STACK) {
                if (dst->anim_stack) dst->anim_stack->layer_ids.push_back(src->id);
            }
            // AnimCurveNode -> AnimLayer
            if (src->type == FBXObjectType::ANIMATION_CURVE_NODE && dst->type == FBXObjectType::ANIMATION_LAYER) {
                if (dst->anim_layer) dst->anim_layer->curve_node_ids.push_back(src->id);
            }
            // Cluster -> Bone (LimbNode Model)
            if (src->type == FBXObjectType::DEFORMER_CLUSTER && dst->type == FBXObjectType::MODEL) {
                if (src->cluster && src->cluster->bone_model_id == 0) {
                    // This OO connection actually points from the cluster's
                    // owning Skin to the Model. The cluster's bone is taken
                    // from a separate Cluster->Model connection below.
                }
            }
            if (dst->type == FBXObjectType::DEFORMER_CLUSTER && src->type == FBXObjectType::MODEL) {
                if (dst->cluster) dst->cluster->bone_model_id = src->id;
            }
        } else if (c.kind == FBXConnection::OP) {
            // Texture -> Material channel (e.g. DiffuseColor)
            if (src->type == FBXObjectType::TEXTURE && dst->type == FBXObjectType::MATERIAL) {
                if (dst->material) dst->material->textures[c.property] = src->id;
            }
            // AnimCurve -> AnimCurveNode (property = "d|X" / "d|Y" / ...)
            if (src->type == FBXObjectType::ANIMATION_CURVE && dst->type == FBXObjectType::ANIMATION_CURVE_NODE) {
                if (dst->anim_curve_node) dst->anim_curve_node->curves[c.property] = src->id;
            }
            // Video -> Texture (typically "Content" or no property)
            if (src->type == FBXObjectType::VIDEO && dst->type == FBXObjectType::TEXTURE) {
                if (dst->texture) dst->texture->video_id = src->id;
            }
        }
    }

    // Identify root Models: Models that have no OO connection to another Model.
    std::unordered_set<FBXObjectId> has_parent_model;
    for (const FBXConnection& c : connections_) {
        if (c.kind != FBXConnection::OO) continue;
        const FBXObject* src = get(c.src);
        const FBXObject* dst = get(c.dst);
        if (!src || !dst) continue;
        if (src->type == FBXObjectType::MODEL && dst->type == FBXObjectType::MODEL) {
            has_parent_model.insert(src->id);
        }
    }
    root_model_ids.clear();
    for (FBXObjectId id : ids_by_type_[FBXObjectType::MODEL]) {
        if (!has_parent_model.count(id)) {
            root_model_ids.push_back(id);
            if (FBXObject* o = const_cast<FBXObject*>(get(id))) {
                if (o->model) o->model->is_root = true;
            }
        }
    }
}

bool FBXScene::load(const FBXDocument& doc, std::string* error) {
    objects_.clear();
    ids_by_type_.clear();
    connections_.clear();
    conn_by_src_.clear();
    conn_by_dst_.clear();
    warnings_.clear();
    root_model_ids.clear();

    file_version = doc.version;

    const FBXNode& root = doc.root;

    if (const FBXNode* ext = root.find_child("FBXHeaderExtension")) {
        if (const FBXNode* c = ext->find_child("Creator")) {
            if (!c->properties.empty()) creator = c->properties[0].as_string();
        }
        if (const FBXNode* c = ext->find_child("CreationTimeStamp")) {
            (void)c; // ignored
        }
        if (const FBXNode* info = ext->find_child("SceneInfo")) {
            Properties70 p = parse_properties70(info->find_child("Properties70"));
            application_name    = p.str("LastSaved|ApplicationName",    application_name);
            application_vendor  = p.str("LastSaved|ApplicationVendor",  application_vendor);
            application_version = p.str("LastSaved|ApplicationVersion", application_version);
        }
    }
    if (const FBXNode* c = root.find_child("Creator")) {
        if (creator.empty() && !c->properties.empty()) creator = c->properties[0].as_string();
    }
    if (const FBXNode* n = root.find_child("GlobalSettings")) parse_global_settings(*n);
    if (const FBXNode* n = root.find_child("Documents"))      parse_documents(*n);
    if (const FBXNode* n = root.find_child("Objects"))        parse_objects(*n);
    if (const FBXNode* n = root.find_child("Connections"))    parse_connections(*n);

    finalize_root_models();

    (void)error;
    return true;
}

// ─── Accessors ─────────────────────────────────────────────

const FBXObject* FBXScene::get(FBXObjectId id) const noexcept {
    auto it = objects_.find(id);
    return (it == objects_.end()) ? nullptr : it->second.get();
}
bool FBXScene::has(FBXObjectId id) const noexcept { return objects_.find(id) != objects_.end(); }

std::vector<FBXObjectId> FBXScene::all_ids() const {
    std::vector<FBXObjectId> r;
    r.reserve(objects_.size());
    for (const auto& kv : objects_) r.push_back(kv.first);
    return r;
}

std::vector<FBXObjectId> FBXScene::ids_of_type(FBXObjectType type) const {
    auto it = ids_by_type_.find(type);
    return (it == ids_by_type_.end()) ? std::vector<FBXObjectId>{} : it->second;
}

std::vector<FBXObjectId> FBXScene::children_of(FBXObjectId id) const {
    std::vector<FBXObjectId> r;
    auto it = conn_by_dst_.find(id);
    if (it == conn_by_dst_.end()) return r;
    for (size_t idx : it->second) r.push_back(connections_[idx].src);
    return r;
}

std::vector<FBXObjectId> FBXScene::parents_of(FBXObjectId id) const {
    std::vector<FBXObjectId> r;
    auto it = conn_by_src_.find(id);
    if (it == conn_by_src_.end()) return r;
    for (size_t idx : it->second) r.push_back(connections_[idx].dst);
    return r;
}

std::vector<FBXObjectId> FBXScene::connected_by_property(FBXObjectId id, const std::string& prop) const {
    std::vector<FBXObjectId> r;
    auto it = conn_by_dst_.find(id);
    if (it == conn_by_dst_.end()) return r;
    for (size_t idx : it->second) {
        const FBXConnection& c = connections_[idx];
        if (c.kind == FBXConnection::OP && c.property == prop) r.push_back(c.src);
    }
    return r;
}

// ─── Triangulation ─────────────────────────────────────────

const FBXGeometry::Triangulated* FBXScene::triangulate(FBXObjectId geometry_id) const {
    const FBXObject* obj = get(geometry_id);
    if (!obj || !obj->geometry) return nullptr;
    FBXGeometry& g = *obj->geometry;
    FBXGeometry::Triangulated& t = g.triangulated_cache;
    if (t.valid) return &t;

    t = FBXGeometry::Triangulated{};

    // Split polygon_vertex_index into polygons using the FBX "XOR -1" rule:
    // the last index of each polygon is encoded as ~index (bitwise NOT).
    std::vector<std::pair<int32_t, int32_t>> polygons;  // (start, count)
    int32_t start = 0;
    for (size_t i = 0; i < g.polygon_vertex_index.size(); ++i) {
        if (g.polygon_vertex_index[i] < 0) {
            polygons.emplace_back(start, static_cast<int32_t>(i - start + 1));
            start = static_cast<int32_t>(i + 1);
        }
    }
    // Tolerate exports that forget to terminate the final polygon; pick it up
    // as a whole trailing run.
    if (start < static_cast<int32_t>(g.polygon_vertex_index.size())) {
        polygons.emplace_back(start, static_cast<int32_t>(g.polygon_vertex_index.size()) - start);
    }

    auto vi = [&](int32_t i) -> int32_t {
        if (i < 0 || i >= static_cast<int32_t>(g.polygon_vertex_index.size())) return -1;
        int32_t v = g.polygon_vertex_index[i];
        return v < 0 ? (~v) : v;
    };

    const int32_t pv_count = static_cast<int32_t>(g.polygon_vertex_index.size());
    const int32_t v_count  = static_cast<int32_t>(g.positions.size());

    // Pick a sensible default src index based on which mapping mode fits the
    // data sizes. Some FBX exporters omit MappingInformationType entirely, or
    // set it to a value we don't recognize. We fall back to whichever of
    // (pv_index, vertex_index) lands in-range for the data size.
    auto fit_src = [&](int32_t pv_index, int32_t poly_index, int32_t vertex_index,
                       int32_t data_size, FBXMappingMode mapping) -> int32_t {
        switch (mapping) {
            case FBXMappingMode::BY_POLYGON_VERTEX: return pv_index;
            case FBXMappingMode::BY_VERTEX:         return vertex_index;
            case FBXMappingMode::BY_POLYGON:        return poly_index;
            case FBXMappingMode::ALL_SAME:          return 0;
            default: break;
        }
        // Heuristic fallback: match by data size.
        if (data_size == pv_count) return pv_index;
        if (data_size == v_count)  return vertex_index;
        if (data_size == 1)        return 0;
        return pv_index;  // final fallback
    };

    // Resolve per-tri-vertex attribute fetch helpers.
    auto resolve_normal = [&](int32_t pv_index, int32_t poly_index, int32_t vertex_index) -> float3 {
        if (g.normals.empty()) return {0, 0, 0};
        const FBXLayerElement<float3>& le = g.normals.front();
        const int32_t data_size = static_cast<int32_t>(le.data.size());
        int32_t src = fit_src(pv_index, poly_index, vertex_index, data_size, le.mapping);
        // IndexToDirect: the data size == unique-value count, index[] size
        // matches the mapping count; look up via index[src] -> data[di].
        if (le.reference == FBXReferenceMode::INDEX_TO_DIRECT && !le.index.empty()) {
            if (src < 0 || src >= static_cast<int32_t>(le.index.size())) {
                // Fallback: try vertex_index if pv_index was chosen but doesn't fit.
                if (vertex_index >= 0 && vertex_index < static_cast<int32_t>(le.index.size())) src = vertex_index;
                else return {0, 0, 0};
            }
            const int32_t di = le.index[src];
            if (di < 0 || di >= data_size) return {0, 0, 0};
            return le.data[di];
        }
        // Direct (or INDEX_TO_DIRECT with empty index): data[src] directly.
        if (src < 0 || src >= data_size) {
            // Final fallback: try vertex_index.
            if (vertex_index >= 0 && vertex_index < data_size) return le.data[vertex_index];
            return {0, 0, 0};
        }
        return le.data[src];
    };
    auto resolve_color = [&](int32_t pv_index, int32_t poly_index, int32_t vertex_index) -> float4 {
        if (g.colors.empty()) return {1, 1, 1, 1};
        const FBXLayerElement<float4>& le = g.colors.front();
        const int32_t data_size = static_cast<int32_t>(le.data.size());
        int32_t src = fit_src(pv_index, poly_index, vertex_index, data_size, le.mapping);
        if (le.reference == FBXReferenceMode::INDEX_TO_DIRECT && !le.index.empty()) {
            if (src < 0 || src >= static_cast<int32_t>(le.index.size())) {
                if (vertex_index >= 0 && vertex_index < static_cast<int32_t>(le.index.size())) src = vertex_index;
                else return {1, 1, 1, 1};
            }
            const int32_t di = le.index[src];
            if (di < 0 || di >= data_size) return {1, 1, 1, 1};
            return le.data[di];
        }
        if (src < 0 || src >= data_size) {
            if (vertex_index >= 0 && vertex_index < data_size) return le.data[vertex_index];
            return {1, 1, 1, 1};
        }
        return le.data[src];
    };
    auto resolve_uv0 = [&](int32_t pv_index, int32_t vertex_index) -> std::pair<float, float> {
        if (g.uv_sets.empty()) return {0, 0};
        const FBXGeometry::UVSet& uv = g.uv_sets.front();
        const int32_t u_size = static_cast<int32_t>(uv.u.size());
        // UVs are almost always per-pv-index (ByPolygonVertex + IndexToDirect),
        // but handle ByVertex and Direct variants as well.
        int32_t src = 0;
        switch (uv.mapping) {
            case FBXMappingMode::BY_POLYGON_VERTEX: src = pv_index; break;
            case FBXMappingMode::BY_VERTEX:         src = vertex_index; break;
            case FBXMappingMode::ALL_SAME:          src = 0; break;
            default:
                // Heuristic: IndexToDirect with index sized to pv_count implies
                // BY_POLYGON_VERTEX; direct with u_size == v_count implies BY_VERTEX.
                if (!uv.index.empty()) {
                    if (static_cast<int32_t>(uv.index.size()) == pv_count) src = pv_index;
                    else if (static_cast<int32_t>(uv.index.size()) == v_count) src = vertex_index;
                    else src = pv_index;
                } else {
                    if (u_size == v_count) src = vertex_index;
                    else src = pv_index;
                }
                break;
        }
        int32_t di = src;
        if (uv.reference == FBXReferenceMode::INDEX_TO_DIRECT && !uv.index.empty()) {
            if (src < 0 || src >= static_cast<int32_t>(uv.index.size())) {
                if (vertex_index >= 0 && vertex_index < static_cast<int32_t>(uv.index.size())) di = uv.index[vertex_index];
                else return {0, 0};
            } else {
                di = uv.index[src];
            }
        }
        if (di < 0 || di >= u_size) return {0, 0};
        return {uv.u[di], uv.v[di]};
    };
    auto resolve_material_per_polygon = [&](int32_t poly_index) -> int32_t {
        if (g.material_indices.data.empty()) return 0;
        if (g.material_indices.mapping == FBXMappingMode::ALL_SAME) return g.material_indices.data[0];
        if (poly_index < 0 || poly_index >= static_cast<int32_t>(g.material_indices.data.size())) return 0;
        return g.material_indices.data[poly_index];
    };

    int32_t out_idx = 0;
    for (size_t pi = 0; pi < polygons.size(); ++pi) {
        int32_t ps = polygons[pi].first;
        int32_t pn = polygons[pi].second;
        if (pn < 3) continue;
        int32_t v0 = vi(ps);
        int32_t mat = resolve_material_per_polygon(static_cast<int32_t>(pi));
        for (int32_t k = 1; k + 1 < pn; ++k) {
            int32_t a_pv = ps;
            int32_t b_pv = ps + k;
            int32_t c_pv = ps + k + 1;
            int32_t a = v0;
            int32_t b = vi(b_pv);
            int32_t c = vi(c_pv);

            auto push = [&](int32_t pv_index, int32_t vertex_index) {
                t.positions.push_back((vertex_index >= 0 && vertex_index < v_count)
                                      ? g.positions[vertex_index]
                                      : float3{});
                t.normals.push_back(resolve_normal(pv_index, static_cast<int32_t>(pi), vertex_index));
                float4 col = resolve_color(pv_index, static_cast<int32_t>(pi), vertex_index);
                t.colors.push_back(col);
                auto uv = resolve_uv0(pv_index, vertex_index);
                t.uv0.push_back(uv.first);
                t.uv0.push_back(uv.second);
                t.original_vertex.push_back(vertex_index);
                t.indices.push_back(out_idx++);
            };

            push(a_pv, a);
            push(b_pv, b);
            push(c_pv, c);
            t.material_per_tri.push_back(mat);
            t.polygon_of_tri.push_back(static_cast<int32_t>(pi));
        }
    }

    t.valid = true;
    return &t;
}

// ─── Transform evaluation ──────────────────────────────────

float4x4 FBXScene::evaluate_local_transform(FBXObjectId model_id) const {
    const FBXObject* obj = get(model_id);
    if (!obj || !obj->model) return mat_identity();
    const FBXModel& m = *obj->model;

    // FBX SDK transform assembly:
    //   M = T * Roff * Rp * Rpre * R * Rpost^-1 * Rp^-1 * Soff * Sp * S * Sp^-1
    const float4x4 T     = mat_translation(m.translation);
    const float4x4 Roff  = mat_translation(m.rotation_offset);
    const float4x4 Rp    = mat_translation(m.rotation_pivot);
    const float4x4 Rpre  = mat_rotation_euler(m.pre_rotation,  FBXRotationOrder::XYZ);
    const float4x4 R     = mat_rotation_euler(m.rotation,      m.rotation_order);
    const float4x4 Rpost = mat_rotation_euler(m.post_rotation, FBXRotationOrder::XYZ);
    const float4x4 Rp_inv    = mat_translation({-m.rotation_pivot.x, -m.rotation_pivot.y, -m.rotation_pivot.z});
    const float4x4 Rpost_inv = mat_inverse_rigid(Rpost);
    const float4x4 Soff  = mat_translation(m.scaling_offset);
    const float4x4 Sp    = mat_translation(m.scaling_pivot);
    const float4x4 S     = mat_scale(m.scaling);
    const float4x4 Sp_inv = mat_translation({-m.scaling_pivot.x, -m.scaling_pivot.y, -m.scaling_pivot.z});

    float4x4 local = mat_mul(T, Roff);
    local = mat_mul(local, Rp);
    local = mat_mul(local, Rpre);
    local = mat_mul(local, R);
    local = mat_mul(local, Rpost_inv);
    local = mat_mul(local, Rp_inv);
    local = mat_mul(local, Soff);
    local = mat_mul(local, Sp);
    local = mat_mul(local, S);
    local = mat_mul(local, Sp_inv);
    return local;
}

float4x4 FBXScene::evaluate_world_transform(FBXObjectId model_id) const {
    const FBXObject* obj = get(model_id);
    if (!obj || !obj->model) return mat_identity();
    // Find the Model parent via OO connections. Walk up iteratively.
    std::vector<FBXObjectId> chain;
    chain.push_back(model_id);
    while (true) {
        FBXObjectId cur = chain.back();
        FBXObjectId parent_model = 0;
        auto it = conn_by_src_.find(cur);
        if (it == conn_by_src_.end()) break;
        for (size_t idx : it->second) {
            const FBXConnection& c = connections_[idx];
            if (c.kind != FBXConnection::OO) continue;
            const FBXObject* d = get(c.dst);
            if (d && d->type == FBXObjectType::MODEL) {
                parent_model = c.dst;
                break;
            }
        }
        if (parent_model == 0) break;
        // Cycle guard (shouldn't happen, but ensure termination).
        if (std::find(chain.begin(), chain.end(), parent_model) != chain.end()) break;
        chain.push_back(parent_model);
    }
    float4x4 world = mat_identity();
    // Compose from root down: world = local_root * local_child * ... (row-vector).
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
        world = mat_mul(evaluate_local_transform(*it), world);
    }
    return world;
}

} // namespace pictor
