// FBX Importer -- Level 4 implementation
//
// Consumes an FBXDocument (raw) + FBXScene (typed) and projects into Pictor
// runtime descriptors (SkeletonDescriptor, AnimationClipDescriptor,
// SkinMeshDescriptor, material_slots, texture_paths).

#include "pictor/animation/fbx_importer.h"
#include "pictor/animation/fbx_document.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>
#include <unordered_set>

namespace pictor {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr float  kDegToRadF = static_cast<float>(kPi / 180.0);

AnimationFormat format_from_doc(const FBXDocument& doc, bool ok) {
    if (!ok) return AnimationFormat::UNKNOWN;
    return doc.is_ascii ? AnimationFormat::FBX_ASCII : AnimationFormat::FBX_BINARY;
}

/// Convert FBX Euler (degrees) to Quaternion honoring rotation order.
///
/// FBX rotation order XYZ means "apply X first, then Y, then Z", i.e.
/// `v' = v * Rx * Ry * Rz` in Pictor's row-vector / row-major matrix
/// convention (see mat_rotation_euler in fbx_scene.cpp).
///
/// Pictor's Hamilton quaternion product `q_a * q_b` applied as `q v q^-1`
/// applies q_b first, then q_a. Equivalently, `(q_a * q_b).to_matrix()` in
/// row-major is `M_b * M_a` (b is the left operand of row-vector composition).
///
/// So the quaternion that reproduces `Rx * Ry * Rz` in row-vector composition
/// is `qz * qy * qx`, **not** `qx * qy * qz`. All orders are simply the
/// reverse of the matrix-order product.
Quaternion euler_deg_to_quat(const float3& deg, FBXRotationOrder order) {
    const float rx = deg.x * kDegToRadF;
    const float ry = deg.y * kDegToRadF;
    const float rz = deg.z * kDegToRadF;
    const Quaternion qx = Quaternion::from_axis_angle({1, 0, 0}, rx);
    const Quaternion qy = Quaternion::from_axis_angle({0, 1, 0}, ry);
    const Quaternion qz = Quaternion::from_axis_angle({0, 0, 1}, rz);
    switch (order) {
        case FBXRotationOrder::XYZ: return qz * qy * qx;
        case FBXRotationOrder::XZY: return qy * qz * qx;
        case FBXRotationOrder::YZX: return qx * qz * qy;
        case FBXRotationOrder::YXZ: return qz * qx * qy;
        case FBXRotationOrder::ZXY: return qy * qx * qz;
        case FBXRotationOrder::ZYX: return qx * qy * qz;
        default:                    return qz * qy * qx;
    }
}

/// Decompose an affine row-major row-vector matrix into T/R/S.
/// Assumes no shear (FBX local transforms have none when pivots/offsets
/// compose cleanly). Row lengths are the per-axis scale; the normalized
/// rows form the rotation basis that `Quaternion::to_matrix()` produces.
void decompose_trs_row_major(const float4x4& m, Transform& out) {
    out.translation = {m.m[3][0], m.m[3][1], m.m[3][2]};

    float3 r0{m.m[0][0], m.m[0][1], m.m[0][2]};
    float3 r1{m.m[1][0], m.m[1][1], m.m[1][2]};
    float3 r2{m.m[2][0], m.m[2][1], m.m[2][2]};
    auto length_of = [](const float3& v) {
        return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    };
    float sx = length_of(r0);
    float sy = length_of(r1);
    float sz = length_of(r2);
    out.scale = {sx, sy, sz};

    auto normed = [](float3 v, float s) -> float3 {
        return s > 1e-8f ? float3{v.x / s, v.y / s, v.z / s} : float3{1, 0, 0};
    };
    r0 = normed(r0, sx);
    r1 = normed(r1, sy);
    r2 = normed(r2, sz);

    // Pictor's Quaternion::to_matrix produces rows:
    //   r0 = (1-2(yy+zz), 2(xy+wz), 2(xz-wy))
    //   r1 = (2(xy-wz), 1-2(xx+zz), 2(yz+wx))
    //   r2 = (2(xz+wy), 2(yz-wx), 1-2(xx+yy))
    // Invert via trace-based algorithm (Shoemake-style, adapted).
    float trace = r0.x + r1.y + r2.z;
    Quaternion q;
    if (trace > 0.0f) {
        float s = std::sqrt(trace + 1.0f) * 2.0f;   // s = 4*w
        q.w = 0.25f * s;
        q.x = (r1.z - r2.y) / s;                    // (yz+wx) - (yz-wx) = 2wx, scaled
        q.y = (r2.x - r0.z) / s;                    // (xz+wy) - (xz-wy) = 2wy
        q.z = (r0.y - r1.x) / s;                    // (xy+wz) - (xy-wz) = 2wz
    } else if (r0.x > r1.y && r0.x > r2.z) {
        float s = std::sqrt(1.0f + r0.x - r1.y - r2.z) * 2.0f; // s = 4*x
        q.x = 0.25f * s;
        q.y = (r0.y + r1.x) / s;
        q.z = (r2.x + r0.z) / s;
        q.w = (r1.z - r2.y) / s;
    } else if (r1.y > r2.z) {
        float s = std::sqrt(1.0f + r1.y - r0.x - r2.z) * 2.0f; // s = 4*y
        q.x = (r0.y + r1.x) / s;
        q.y = 0.25f * s;
        q.z = (r1.z + r2.y) / s;
        q.w = (r2.x - r0.z) / s;
    } else {
        float s = std::sqrt(1.0f + r2.z - r0.x - r1.y) * 2.0f; // s = 4*z
        q.x = (r2.x + r0.z) / s;
        q.y = (r1.z + r2.y) / s;
        q.z = 0.25f * s;
        q.w = (r0.y - r1.x) / s;
    }
    out.rotation = q.normalized();
}

/// Rigid-transform inverse (rotation transpose + -R^T * t).
float4x4 inverse_rigid_approx(const float4x4& m) {
    float4x4 r = float4x4::identity();
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            r.m[i][j] = m.m[j][i];
    float tx = m.m[3][0], ty = m.m[3][1], tz = m.m[3][2];
    r.m[3][0] = -(r.m[0][0] * tx + r.m[1][0] * ty + r.m[2][0] * tz);
    r.m[3][1] = -(r.m[0][1] * tx + r.m[1][1] * ty + r.m[2][1] * tz);
    r.m[3][2] = -(r.m[0][2] * tx + r.m[1][2] * ty + r.m[2][2] * tz);
    return r;
}

/// Linear sample from an FBXAnimCurve at t_seconds.
float sample_curve(const FBXAnimCurve* curve, double t_seconds, float fallback) {
    if (!curve || curve->key_time.empty()) return fallback;
    const size_t n = curve->key_time.size();
    if (curve->key_value.size() != n) return fallback;
    const double first = FBXScene::ktime_to_seconds(curve->key_time.front());
    const double last  = FBXScene::ktime_to_seconds(curve->key_time.back());
    if (t_seconds <= first) return curve->key_value.front();
    if (t_seconds >= last)  return curve->key_value.back();
    size_t lo = 0, hi = n - 1;
    while (hi - lo > 1) {
        size_t mid = (lo + hi) / 2;
        double tm = FBXScene::ktime_to_seconds(curve->key_time[mid]);
        if (tm <= t_seconds) lo = mid; else hi = mid;
    }
    const double t0 = FBXScene::ktime_to_seconds(curve->key_time[lo]);
    const double t1 = FBXScene::ktime_to_seconds(curve->key_time[hi]);
    const float  v0 = curve->key_value[lo];
    const float  v1 = curve->key_value[hi];
    const float  f  = (t1 > t0) ? static_cast<float>((t_seconds - t0) / (t1 - t0)) : 0.0f;
    return v0 + (v1 - v0) * f;
}

} // namespace

// ─── FBXImportResult helpers ──────────────────────────────

const FBXObject* FBXImportResult::get_resource(FBXObjectId id) const noexcept {
    return scene ? scene->get(id) : nullptr;
}
std::vector<FBXObjectId> FBXImportResult::all_resource_ids() const {
    return scene ? scene->all_ids() : std::vector<FBXObjectId>{};
}
std::vector<FBXObjectId> FBXImportResult::resource_ids_of_type(FBXObjectType type) const {
    return scene ? scene->ids_of_type(type) : std::vector<FBXObjectId>{};
}

ModelDescriptor FBXImportResult::to_model_descriptor(const std::string& name) const {
    ModelDescriptor md;
    md.name   = name;
    md.format = ModelFormat::FBX;

    md.rig.name      = skeleton.name.empty() ? name + "_rig" : skeleton.name;
    md.rig.bones     = skeleton.bones;
    md.rig.ik_chains = ik_chains;

    md.skin_meshes     = skin_meshes;
    md.animation_clips = clips;
    md.material_slots  = material_slots;
    md.texture_paths   = texture_paths;
    return md;
}

// ─── FBXImporter ─────────────────────────────────────────

AnimationFormat FBXImporter::detect_format(const uint8_t* data, size_t size) {
    if (!data || size == 0) return AnimationFormat::UNKNOWN;
    if (FBXDocument::looks_binary(data, size)) return AnimationFormat::FBX_BINARY;
    std::string head(reinterpret_cast<const char*>(data),
                     std::min<size_t>(size, 256));
    if (head.find("FBXHeaderExtension") != std::string::npos) return AnimationFormat::FBX_ASCII;
    if (head.find("FBXVersion")         != std::string::npos) return AnimationFormat::FBX_ASCII;
    if (head.find("; FBX")              != std::string::npos) return AnimationFormat::FBX_ASCII;
    return AnimationFormat::UNKNOWN;
}

FBXImportResult FBXImporter::import_file(const std::string& path) const {
    FBXImportResult out;
    FBXDocument doc;
    std::string err;
    if (!doc.load_file(path, &err)) {
        out.error_message = err.empty() ? "FBX load failed: " + path : err;
        return out;
    }
    out.detected_format = format_from_doc(doc, true);
    out.scene = std::make_shared<FBXScene>();
    if (!out.scene->load(doc, &err)) {
        out.error_message = err.empty() ? "FBX scene build failed" : err;
        return out;
    }
    project(*out.scene, out);
    out.success = true;
    return out;
}

FBXImportResult FBXImporter::import_memory(const uint8_t* data, size_t size) const {
    FBXImportResult out;
    FBXDocument doc;
    std::string err;
    if (!doc.parse(data, size, &err)) {
        out.error_message = err.empty() ? "FBX parse failed" : err;
        return out;
    }
    out.detected_format = format_from_doc(doc, true);
    out.scene = std::make_shared<FBXScene>();
    if (!out.scene->load(doc, &err)) {
        out.error_message = err.empty() ? "FBX scene build failed" : err;
        return out;
    }
    project(*out.scene, out);
    out.success = true;
    return out;
}

void FBXImporter::project(const FBXScene& scene, FBXImportResult& out) const {
    build_skeleton(scene, out);
    build_clips(scene, out);
    build_skin_meshes(scene, out);
    collect_materials(scene, out);
}

// ─── Skeleton ─────────────────────────────────────────────

void FBXImporter::build_skeleton(const FBXScene& scene, FBXImportResult& out) const {
    std::vector<FBXObjectId> limb_ids;
    for (FBXObjectId mid : scene.ids_of_type(FBXObjectType::MODEL)) {
        const FBXObject* obj = scene.get(mid);
        if (!obj || !obj->model) continue;
        const std::string& st = obj->model->sub_type;
        if (st == "LimbNode" || st == "Limb" || st == "Root" || st == "Null") {
            limb_ids.push_back(mid);
        }
    }
    if (limb_ids.empty()) return;

    std::unordered_map<FBXObjectId, int32_t> id_to_index;
    id_to_index.reserve(limb_ids.size());
    for (size_t i = 0; i < limb_ids.size(); ++i) id_to_index[limb_ids[i]] = static_cast<int32_t>(i);

    out.skeleton.name = "fbx_skeleton";
    out.skeleton.bones.resize(limb_ids.size());

    std::unordered_map<FBXObjectId, float4x4> bone_to_bind;
    for (FBXObjectId cid : scene.ids_of_type(FBXObjectType::DEFORMER_CLUSTER)) {
        const FBXObject* c = scene.get(cid);
        if (!c || !c->cluster) continue;
        bone_to_bind[c->cluster->bone_model_id] = c->cluster->transform_link;
    }

    for (size_t i = 0; i < limb_ids.size(); ++i) {
        FBXObjectId id = limb_ids[i];
        const FBXObject* obj = scene.get(id);
        Bone& b = out.skeleton.bones[i];
        b.name = obj ? obj->name : std::string{};
        b.parent_index = -1;

        for (FBXObjectId pid : scene.parents_of(id)) {
            const FBXObject* po = scene.get(pid);
            if (!po || !po->model) continue;
            auto it = id_to_index.find(pid);
            if (it != id_to_index.end()) { b.parent_index = it->second; break; }
        }

        if (obj && obj->model) {
            const float4x4 local = scene.evaluate_local_transform(id);
            decompose_trs_row_major(local, b.bind_pose);
        }

        auto bt = bone_to_bind.find(id);
        if (bt != bone_to_bind.end()) {
            b.inverse_bind_matrix = inverse_rigid_approx(bt->second);
        } else {
            b.inverse_bind_matrix = inverse_rigid_approx(scene.evaluate_world_transform(id));
        }
    }
}

// ─── Animation clips ──────────────────────────────────────

void FBXImporter::build_clips(const FBXScene& scene, FBXImportResult& out) const {
    std::unordered_map<FBXObjectId, uint32_t> bone_target;
    for (size_t i = 0; i < out.skeleton.bones.size(); ++i) {
        const std::string& bone_name = out.skeleton.bones[i].name;
        for (FBXObjectId mid : scene.ids_of_type(FBXObjectType::MODEL)) {
            const FBXObject* m = scene.get(mid);
            if (m && m->name == bone_name) {
                bone_target[mid] = static_cast<uint32_t>(i);
                break;
            }
        }
    }

    for (FBXObjectId stack_id : scene.ids_of_type(FBXObjectType::ANIMATION_STACK)) {
        const FBXObject* stack_obj = scene.get(stack_id);
        if (!stack_obj || !stack_obj->anim_stack) continue;
        const FBXAnimStack& stack = *stack_obj->anim_stack;

        AnimationClipDescriptor clip;
        clip.name        = stack.name.empty() ? "clip" : stack.name;
        clip.wrap_mode   = WrapMode::LOOP;
        clip.sample_rate = 30.0f;

        double duration = FBXScene::ktime_to_seconds(stack.local_stop - stack.local_start);
        if (duration <= 0.0) duration = FBXScene::ktime_to_seconds(stack.reference_stop - stack.reference_start);
        if (duration <= 0.0) duration = 1.0;
        clip.duration = static_cast<float>(duration);

        std::vector<FBXObjectId> curve_nodes;
        for (FBXObjectId layer_id : stack.layer_ids) {
            const FBXObject* layer_obj = scene.get(layer_id);
            if (!layer_obj || !layer_obj->anim_layer) continue;
            for (FBXObjectId cn : layer_obj->anim_layer->curve_node_ids) curve_nodes.push_back(cn);
        }

        for (FBXObjectId cn_id : curve_nodes) {
            const FBXObject* cn_obj = scene.get(cn_id);
            if (!cn_obj || !cn_obj->anim_curve_node) continue;
            const FBXAnimCurveNode& cn = *cn_obj->anim_curve_node;

            // Find the Model this CurveNode is bound to (OP connection src=cn_id).
            FBXObjectId model_id = 0;
            std::string prop_name;
            for (const FBXConnection& c : scene.connections()) {
                if (c.kind == FBXConnection::OP && c.src == cn_id) {
                    const FBXObject* dst = scene.get(c.dst);
                    if (dst && dst->type == FBXObjectType::MODEL) {
                        model_id  = c.dst;
                        prop_name = c.property;
                        break;
                    }
                }
            }
            if (model_id == 0) continue;
            auto target_it = bone_target.find(model_id);
            if (target_it == bone_target.end()) continue;

            ChannelTarget ct;
            if      (prop_name == "Lcl Translation") ct = ChannelTarget::TRANSLATION;
            else if (prop_name == "Lcl Rotation")    ct = ChannelTarget::ROTATION;
            else if (prop_name == "Lcl Scaling")     ct = ChannelTarget::SCALE;
            else continue;

            auto fetch = [&](const std::string& key) -> const FBXAnimCurve* {
                auto it = cn.curves.find(key);
                if (it == cn.curves.end()) return nullptr;
                const FBXObject* o = scene.get(it->second);
                return (o && o->anim_curve) ? o->anim_curve.get() : nullptr;
            };
            const FBXAnimCurve* cx = fetch("d|X");
            const FBXAnimCurve* cy = fetch("d|Y");
            const FBXAnimCurve* cz = fetch("d|Z");
            if (!cx && !cy && !cz) continue;

            std::vector<int64_t> merged;
            auto merge_from = [&](const FBXAnimCurve* c) {
                if (!c) return;
                merged.insert(merged.end(), c->key_time.begin(), c->key_time.end());
            };
            merge_from(cx); merge_from(cy); merge_from(cz);
            std::sort(merged.begin(), merged.end());
            merged.erase(std::unique(merged.begin(), merged.end()), merged.end());
            if (merged.empty()) continue;

            auto dflt = [&](const std::string& key) -> float {
                auto it = cn.default_values.find(key);
                return (it == cn.default_values.end()) ? 0.0f : static_cast<float>(it->second);
            };
            const float dx = dflt("d|X");
            const float dy = dflt("d|Y");
            const float dz = dflt("d|Z");

            AnimationChannel ch;
            ch.target_index = target_it->second;
            ch.target       = ct;
            ch.interpolation = InterpolationMode::LINEAR;
            ch.keyframes.reserve(merged.size());

            for (int64_t kt : merged) {
                double sec = FBXScene::ktime_to_seconds(kt);
                float x = sample_curve(cx, sec, dx);
                float y = sample_curve(cy, sec, dy);
                float z = sample_curve(cz, sec, dz);
                Keyframe k;
                k.time = static_cast<float>(sec);
                if (ct == ChannelTarget::ROTATION) {
                    // Feed the sampled Euler into the full FBX local
                    // transform formula (pre/post/pivots honoured), then
                    // decompose back to T/R/S so the stored quat reproduces
                    // the correct local rotation via Transform::to_matrix().
                    const float3 r_anim{x, y, z};
                    const float4x4 local =
                        scene.evaluate_local_transform_with_anim(model_id, nullptr, &r_anim, nullptr);
                    Transform tmp;
                    decompose_trs_row_major(local, tmp);
                    k.value[0] = tmp.rotation.x;
                    k.value[1] = tmp.rotation.y;
                    k.value[2] = tmp.rotation.z;
                    k.value[3] = tmp.rotation.w;
                } else {
                    k.value[0] = x; k.value[1] = y; k.value[2] = z; k.value[3] = 0.0f;
                }
                ch.keyframes.push_back(k);
            }
            clip.channels.push_back(std::move(ch));
        }

        if (!clip.channels.empty()) out.clips.push_back(std::move(clip));
    }
}

// ─── Skinned meshes ───────────────────────────────────────

void FBXImporter::build_skin_meshes(const FBXScene& scene, FBXImportResult& out) const {
    std::unordered_map<std::string, uint32_t> bone_by_name;
    for (size_t i = 0; i < out.skeleton.bones.size(); ++i) {
        bone_by_name[out.skeleton.bones[i].name] = static_cast<uint32_t>(i);
    }

    for (FBXObjectId gid : scene.ids_of_type(FBXObjectType::GEOMETRY)) {
        const FBXObject* g = scene.get(gid);
        if (!g || !g->geometry) continue;
        const FBXGeometry::Triangulated* tri = scene.triangulate(gid);
        if (!tri || !tri->valid) continue;

        std::string mesh_name = g->name;
        for (FBXObjectId pid : scene.parents_of(gid)) {
            const FBXObject* po = scene.get(pid);
            if (po && po->type == FBXObjectType::MODEL) {
                if (mesh_name.empty()) mesh_name = po->name;
                break;
            }
        }

        SkinMeshDescriptor mesh;
        mesh.name         = mesh_name.empty() ? ("mesh_" + std::to_string(gid)) : mesh_name;
        mesh.vertex_count = static_cast<uint32_t>(tri->positions.size());
        mesh.index_count  = static_cast<uint32_t>(tri->indices.size());
        mesh.index_32bit  = true;
        mesh.index_data      = tri->indices.empty() ? nullptr : tri->indices.data();
        mesh.index_data_size = tri->indices.size() * sizeof(int32_t);
        // vertex_data is left null: the triangulated cache owns the raw attrs.

        // Per-vertex skin weights.
        const uint32_t orig_vcount = static_cast<uint32_t>(g->geometry->positions.size());
        std::vector<SkinWeight> weights_orig(orig_vcount);

        auto insert_weight = [&](SkinWeight& sw, uint32_t bone, float w) {
            int min_slot = 0;
            for (int s = 1; s < 4; ++s)
                if (sw.weights[s] < sw.weights[min_slot]) min_slot = s;
            if (w > sw.weights[min_slot]) {
                sw.weights[min_slot]      = w;
                sw.bone_indices[min_slot] = bone;
            }
        };

        for (FBXObjectId skin_id : scene.children_of(gid)) {
            const FBXObject* s = scene.get(skin_id);
            if (!s || !s->skin) continue;
            for (FBXObjectId cid : s->skin->cluster_ids) {
                const FBXObject* co = scene.get(cid);
                if (!co || !co->cluster) continue;
                const FBXCluster& cl = *co->cluster;
                const FBXObject* bone_obj = scene.get(cl.bone_model_id);
                if (!bone_obj) continue;
                auto bi = bone_by_name.find(bone_obj->name);
                if (bi == bone_by_name.end()) continue;
                uint32_t bone_index = bi->second;
                size_t n = std::min(cl.indices.size(), cl.weights.size());
                for (size_t k = 0; k < n; ++k) {
                    int32_t vi = cl.indices[k];
                    if (vi < 0 || static_cast<uint32_t>(vi) >= orig_vcount) continue;
                    insert_weight(weights_orig[vi], bone_index, static_cast<float>(cl.weights[k]));
                }
            }
        }

        for (SkinWeight& sw : weights_orig) {
            float sum = sw.weights[0] + sw.weights[1] + sw.weights[2] + sw.weights[3];
            if (sum <= 0.0f) {
                sw.bone_indices[0] = 0;
                sw.weights[0]      = 1.0f;
            } else {
                for (int k = 0; k < 4; ++k) sw.weights[k] /= sum;
            }
        }

        mesh.skin_weights.resize(tri->positions.size());
        for (size_t i = 0; i < tri->positions.size(); ++i) {
            int32_t orig = (i < tri->original_vertex.size()) ? tri->original_vertex[i] : 0;
            if (orig < 0 || static_cast<uint32_t>(orig) >= orig_vcount) {
                SkinWeight sw;
                sw.bone_indices[0] = 0; sw.weights[0] = 1.0f;
                mesh.skin_weights[i] = sw;
            } else {
                mesh.skin_weights[i] = weights_orig[orig];
            }
        }

        // Morph targets.
        for (FBXObjectId bs_id : scene.children_of(gid)) {
            const FBXObject* bs_obj = scene.get(bs_id);
            if (!bs_obj || bs_obj->type != FBXObjectType::DEFORMER_BLENDSHAPE) continue;
            if (!bs_obj->blendshape) continue;
            for (FBXObjectId ch_id : bs_obj->blendshape->channel_ids) {
                const FBXObject* ch_obj = scene.get(ch_id);
                if (!ch_obj || !ch_obj->blendshape_channel) continue;
                for (FBXObjectId sg_id : ch_obj->blendshape_channel->shape_ids) {
                    const FBXObject* sg_obj = scene.get(sg_id);
                    if (!sg_obj || !sg_obj->shape_geometry) continue;
                    mesh.morph_target_names.push_back(ch_obj->blendshape_channel->name);
                    const FBXShapeGeometry& sg = *sg_obj->shape_geometry;
                    std::vector<float> deltas(static_cast<size_t>(mesh.vertex_count) * 3, 0.0f);
                    for (size_t i = 0; i < tri->positions.size(); ++i) {
                        int32_t orig = (i < tri->original_vertex.size()) ? tri->original_vertex[i] : -1;
                        if (orig < 0) continue;
                        for (size_t k = 0; k < sg.indices.size(); ++k) {
                            if (sg.indices[k] == orig && k < sg.position_deltas.size()) {
                                deltas[i * 3 + 0] = sg.position_deltas[k].x;
                                deltas[i * 3 + 1] = sg.position_deltas[k].y;
                                deltas[i * 3 + 2] = sg.position_deltas[k].z;
                                break;
                            }
                        }
                    }
                    mesh.morph_deltas.insert(mesh.morph_deltas.end(), deltas.begin(), deltas.end());
                }
            }
        }

        out.skin_meshes.push_back(std::move(mesh));
    }
}

// ─── Material / texture collection ────────────────────────

void FBXImporter::collect_materials(const FBXScene& scene, FBXImportResult& out) const {
    for (FBXObjectId mid : scene.ids_of_type(FBXObjectType::MATERIAL)) {
        const FBXObject* m = scene.get(mid);
        if (!m || !m->material) continue;
        out.material_slots.push_back(m->material->name);
    }
    std::unordered_set<std::string> seen;
    for (FBXObjectId tid : scene.ids_of_type(FBXObjectType::TEXTURE)) {
        const FBXObject* t = scene.get(tid);
        if (!t || !t->texture) continue;
        const std::string& path = !t->texture->file_name.empty() ? t->texture->file_name
                                                                  : t->texture->relative_filename;
        if (path.empty()) continue;
        if (seen.insert(path).second) out.texture_paths.push_back(path);
    }
}

} // namespace pictor
