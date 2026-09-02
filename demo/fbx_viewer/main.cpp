// Pictor FBX Viewer Demo
//
// End-to-end pipeline:
//   1. Load a model directory (default: fbx/model1):
//        - model.fbx         — geometry, skeleton, materials, embedded clips
//        - texture/*.tga|png — diffuse textures referenced by materials
//        - animation/*.fbx   — extra animation clips bound to the same skeleton
//   2. Build per-material submeshes and upload diffuse textures to Vulkan.
//   3. Register skeleton + all clips with AnimationSystem.
//   4. Create a skinning pipeline with the textured lit shader
//      (demo/fbx_viewer/shaders/model.{vert,frag}).
//   5. Each frame: push updated bone matrices, bind the right texture per
//      submesh, and draw.
//
// Keyboard:
//   SPACE / N : cycle to next animation clip
//   P         : previous animation clip
//   R         : restart current clip
//   F         : toggle shell fur (demo/fbx_viewer/shaders/fur_shell.{vert,frag})
//   [ ] - = 9 0 , . O : fur tuning (see FBXViewer::on_fur_key)
//   T / Y / I / K : rope binding toggle / pattern / tightness (nawa_binding.h)
//
// Usage:
//   pictor_fbx_viewer [path]  [shader_dir]
// `path` can be a model directory (preferred) or a single .fbx file.
// If omitted, `fbx/model1` is used.

#include "pictor/animation/fbx_importer.h"
#include "pictor/animation/fbx_scene.h"
#include "pictor/animation/animation_system.h"
#include "pictor/animation/skinned_vertex.h"
#include "pictor/core/types.h"
#include "pictor/surface/glfw_surface_provider.h"
#include "pictor/surface/vulkan_context.h"

#include "frame_capture.h"
#include "eye_locator.h"
#include "fur_shell_pass.h"
#include "nawa_binding.h"
#include "rope_pass.h"
#include "tear_pass.h"
#include "textured_skinned_vertex.h"
#include "vk_buffer_util.h"

#include "stb_image.h"

#include <GLFW/glfw3.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits>
#include <string>
#include <unordered_map>
#include <vector>

#ifndef PICTOR_HAS_VULKAN
int main() {
    std::fprintf(stderr, "This demo requires Vulkan (PICTOR_HAS_VULKAN).\n");
    return 1;
}
#else

using namespace pictor;
using namespace pictor_fbx_viewer;
namespace fs = std::filesystem;

// ============================================================
// Math helpers (column-major 4x4, stored as float[16])
// ============================================================

namespace {

void mat4_identity(float* m) {
    std::memset(m, 0, 16 * sizeof(float));
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

void mat4_look_at(float* out, const float* eye, const float* center, const float* up) {
    float fx = center[0] - eye[0], fy = center[1] - eye[1], fz = center[2] - eye[2];
    float fl = std::sqrt(fx*fx + fy*fy + fz*fz);
    if (fl > 0) { fx /= fl; fy /= fl; fz /= fl; }
    float sx = fy*up[2] - fz*up[1], sy = fz*up[0] - fx*up[2], sz = fx*up[1] - fy*up[0];
    float sl = std::sqrt(sx*sx + sy*sy + sz*sz);
    if (sl > 0) { sx /= sl; sy /= sl; sz /= sl; }
    float ux = sy*fz - sz*fy, uy = sz*fx - sx*fz, uz = sx*fy - sy*fx;
    mat4_identity(out);
    out[0] = sx;  out[4] = sy;  out[8]  = sz;  out[12] = -(sx*eye[0]+sy*eye[1]+sz*eye[2]);
    out[1] = ux;  out[5] = uy;  out[9]  = uz;  out[13] = -(ux*eye[0]+uy*eye[1]+uz*eye[2]);
    out[2] = -fx; out[6] = -fy; out[10] = -fz; out[14] =  (fx*eye[0]+fy*eye[1]+fz*eye[2]);
}

void mat4_perspective(float* out, float fovy_rad, float aspect, float near_z, float far_z) {
    std::memset(out, 0, 16 * sizeof(float));
    float f = 1.0f / std::tan(fovy_rad * 0.5f);
    out[0]  = f / aspect;
    out[5]  = -f;                                 // Vulkan Y-flip
    out[10] = far_z / (near_z - far_z);
    out[11] = -1.0f;
    out[14] = (near_z * far_z) / (near_z - far_z);
}

/// Pack a Pictor float4x4 into a GLSL mat4 (std430 column-major).
///
/// Pictor stores matrices as row-vector / row-major (translation at
/// m.m[3][*], `v_row * M`). GLSL mat4 with column-major layout expects
/// column-vector semantics (`M * v_col`, translation at M[0..2][3] stored
/// at flat[12..14]).
///
/// For the same spatial transformation, the column-vector matrix is the
/// transpose of the row-vector matrix. A naive index copy
/// (`out[c*4+r] = m.m[r][c]`) would leave the translation in GLSL row 3
/// instead of column 3 — the skinning translations would vanish and the
/// homogeneous `w` would be corrupted. Transposing on upload (`m.m[c][r]`)
/// puts Pictor row `i` into GLSL column `i`, yielding the correct
/// column-vector matrix.
void pictor_mat_to_glsl(float* out, const pictor::float4x4& m) {
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            out[c * 4 + r] = m.m[c][r];
}

// ============================================================
// Minimal profiler — scoped timers that print at scope exit and
// accumulate totals into a static table for the final summary.
// ============================================================

struct ProfileEntry {
    const char* label = nullptr;
    double      total_ms = 0.0;
    uint32_t    calls = 0;
};

struct Profiler {
    static Profiler& instance() { static Profiler p; return p; }

    void record(const char* label, double ms) {
        for (auto& e : entries_) {
            if (e.label == label) { e.total_ms += ms; ++e.calls; return; }
        }
        entries_.push_back({label, ms, 1});
    }

    void print(const char* title) const {
        std::printf("─── %s ─────────────────────────────\n", title);
        double total = 0;
        for (const auto& e : entries_) {
            std::printf("[prof] %-28s %8.2f ms  (%u call%s)\n",
                        e.label, e.total_ms, e.calls, e.calls == 1 ? "" : "s");
            total += e.total_ms;
        }
        std::printf("[prof] %-28s %8.2f ms\n", "= cumulative", total);
    }

private:
    std::vector<ProfileEntry> entries_;
};

struct ScopedTimer {
    const char* label;
    std::chrono::steady_clock::time_point t0;
    explicit ScopedTimer(const char* l)
        : label(l), t0(std::chrono::steady_clock::now()) {}
    ~ScopedTimer() {
        auto ms = std::chrono::duration<double, std::milli>(
                      std::chrono::steady_clock::now() - t0).count();
        Profiler::instance().record(label, ms);
    }
};

#define PROF_CONCAT_(a, b) a##b
#define PROF_CONCAT(a, b)  PROF_CONCAT_(a, b)
#define PROF_SCOPE(name)   ScopedTimer PROF_CONCAT(_st_, __LINE__)(name)

// TexturedSkinnedVertex + TSV_* offsets live in textured_skinned_vertex.h
// (shared with fur_shell_pass.cpp).

// ============================================================
// PackedMesh — every renderable geometry in the scene is packed
// into a single vertex+index buffer; each submesh carries a direct
// FBX material id (or 0 if untextured) so texture lookup is trivial.
// ============================================================

struct SubMesh {
    uint32_t    index_start = 0;
    uint32_t    index_count = 0;
    FBXObjectId material_id = 0;   // 0 = no material (transient; cleared after resolve)
    std::string texture_basename;  // resolved diffuse texture filename, "" = fallback
    std::string debug_name;        // geometry or model name, for logging
};

struct PackedMesh {
    std::vector<TexturedSkinnedVertex> vertices;
    std::vector<uint32_t>              indices;
    std::vector<SubMesh>               submeshes;
    float3                             center{};
    float                              radius = 1.0f;
};

/// Find the FBX Model that owns `geometry_id` (via OO parent).
FBXObjectId find_parent_model(const FBXScene& scene, FBXObjectId geometry_id) {
    for (FBXObjectId pid : scene.parents_of(geometry_id)) {
        const FBXObject* po = scene.get(pid);
        if (po && po->type == FBXObjectType::MODEL) return pid;
    }
    return 0;
}

/// Ordered list of Material children of a Model (order = material-slot index).
std::vector<FBXObjectId> ordered_materials_of_model(const FBXScene& scene, FBXObjectId model_id) {
    std::vector<FBXObjectId> out;
    if (!model_id) return out;
    for (FBXObjectId cid : scene.children_of(model_id)) {
        const FBXObject* co = scene.get(cid);
        if (co && co->type == FBXObjectType::MATERIAL) out.push_back(cid);
    }
    return out;
}

/// Walk the Model hierarchy from `model_id` up toward the root, returning
/// the first ancestor (inclusive) whose name matches a bone in the given
/// skeleton map. Used to pick a fallback bone for geometries that have no
/// skin deformer (e.g. static face parts parented under the head bone);
/// those sub-meshes should inherit the pose of their nearest bone ancestor.
/// Returns UINT32_MAX if nothing in the chain is a bone.
static uint32_t find_ancestor_bone(const FBXScene& scene,
                                   FBXObjectId model_id,
                                   const std::unordered_map<std::string, uint32_t>& bone_by_name)
{
    FBXObjectId cur = model_id;
    while (cur != 0) {
        const FBXObject* obj = scene.get(cur);
        if (obj) {
            auto it = bone_by_name.find(obj->name);
            if (it != bone_by_name.end()) return it->second;
        }
        FBXObjectId next = 0;
        for (FBXObjectId pid : scene.parents_of(cur)) {
            const FBXObject* po = scene.get(pid);
            if (po && po->type == FBXObjectType::MODEL) { next = pid; break; }
        }
        if (next == cur || next == 0) break;
        cur = next;
    }
    return UINT32_MAX;
}

/// Pack a single triangulated geometry into the output mesh at the current
/// offsets, emitting one SubMesh per distinct material slot.
static void pack_one_geometry(const FBXScene& scene,
                              FBXObjectId gid,
                              const SkeletonDescriptor& skeleton,
                              PackedMesh& out)
{
    const FBXObject* geo_obj = scene.get(gid);
    if (!geo_obj || !geo_obj->geometry) return;
    const FBXGeometry::Triangulated* tri = scene.triangulate(gid);
    if (!tri || !tri->valid || tri->positions.empty() || tri->indices.empty()) return;
    const FBXGeometry& g = *geo_obj->geometry;

    const FBXObjectId              model_id  = find_parent_model(scene, gid);
    const std::vector<FBXObjectId> slot_mats = ordered_materials_of_model(scene, model_id);
    const FBXObject*               model_obj = scene.get(model_id);
    const std::string              dbg_name  = !geo_obj->name.empty()
                                               ? geo_obj->name
                                               : (model_obj ? model_obj->name : std::string("geometry"));

    // ── Skin weights in original-vertex space, expanded to tri-vertices. ──
    const uint32_t orig_vcount = static_cast<uint32_t>(g.positions.size());
    std::unordered_map<std::string, uint32_t> bone_by_name;
    for (size_t i = 0; i < skeleton.bones.size(); ++i) {
        bone_by_name[skeleton.bones[i].name] = static_cast<uint32_t>(i);
    }

    // Fallback bone: for geometries with no skin deformer (e.g. static
    // face parts in UnityChan) we follow the parent Model hierarchy until
    // we find the first bone and treat the whole sub-mesh as rigidly
    // bound to it. Vertices without any cluster weight from the skin pass
    // use the same fallback so they follow their ancestor bone rather
    // than snapping to the world root.
    const uint32_t ancestor_bone = find_ancestor_bone(scene, model_id, bone_by_name);
    const uint32_t fallback_bone = (ancestor_bone != UINT32_MAX) ? ancestor_bone : 0u;

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
    bool has_any_skin = false;
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
            const uint32_t bone_index = bi->second;
            const size_t n = std::min(cl.indices.size(), cl.weights.size());
            for (size_t k = 0; k < n; ++k) {
                const int32_t vi = cl.indices[k];
                if (vi < 0 || static_cast<uint32_t>(vi) >= orig_vcount) continue;
                insert_weight(weights_orig[vi], bone_index, static_cast<float>(cl.weights[k]));
                has_any_skin = true;
            }
        }
    }
    for (SkinWeight& sw : weights_orig) {
        float sum = sw.weights[0] + sw.weights[1] + sw.weights[2] + sw.weights[3];
        if (sum <= 0.0f) {
            sw.bone_indices[0] = fallback_bone;
            sw.weights[0]      = 1.0f;
        } else {
            for (int k = 0; k < 4; ++k) sw.weights[k] /= sum;
        }
    }
    (void)has_any_skin;

    const size_t vcount = tri->positions.size();
    std::vector<SkinWeight> weights_tri(vcount);
    for (size_t i = 0; i < vcount; ++i) {
        int32_t orig = (i < tri->original_vertex.size()) ? tri->original_vertex[i] : -1;
        if (orig >= 0 && static_cast<uint32_t>(orig) < orig_vcount) {
            weights_tri[i] = weights_orig[orig];
        } else {
            weights_tri[i].bone_indices[0] = fallback_bone;
            weights_tri[i].weights[0]      = 1.0f;
        }
    }

    std::vector<float3> normals = tri->normals;
    if (normals.size() != vcount) {
        normals.assign(vcount, {0, 1, 0});
        for (size_t i = 0; i + 2 < tri->indices.size(); i += 3) {
            int32_t ia = tri->indices[i + 0];
            int32_t ib = tri->indices[i + 1];
            int32_t ic = tri->indices[i + 2];
            if (ia < 0 || static_cast<size_t>(ia) >= vcount ||
                ib < 0 || static_cast<size_t>(ib) >= vcount ||
                ic < 0 || static_cast<size_t>(ic) >= vcount) continue;
            const float3& A = tri->positions[ia];
            const float3& B = tri->positions[ib];
            const float3& C = tri->positions[ic];
            float3 e1{B.x - A.x, B.y - A.y, B.z - A.z};
            float3 e2{C.x - A.x, C.y - A.y, C.z - A.z};
            float3 n{e1.y*e2.z - e1.z*e2.y, e1.z*e2.x - e1.x*e2.z, e1.x*e2.y - e1.y*e2.x};
            float l = std::sqrt(n.x*n.x + n.y*n.y + n.z*n.z);
            if (l > 0) { n.x/=l; n.y/=l; n.z/=l; }
            normals[ia] = n; normals[ib] = n; normals[ic] = n;
        }
    }

    // ── Append vertices ──────────────────────────────────────
    const uint32_t vertex_base = static_cast<uint32_t>(out.vertices.size());
    out.vertices.resize(vertex_base + vcount);
    for (size_t i = 0; i < vcount; ++i) {
        TexturedSkinnedVertex& v = out.vertices[vertex_base + i];
        v.position[0] = tri->positions[i].x;
        v.position[1] = tri->positions[i].y;
        v.position[2] = tri->positions[i].z;
        v.normal[0]   = normals[i].x;
        v.normal[1]   = normals[i].y;
        v.normal[2]   = normals[i].z;
        if (i * 2 + 1 < tri->uv0.size()) {
            v.uv[0] = tri->uv0[i * 2 + 0];
            v.uv[1] = 1.0f - tri->uv0[i * 2 + 1];
        } else {
            v.uv[0] = 0.0f; v.uv[1] = 0.0f;
        }
        for (int k = 0; k < 4; ++k) {
            v.joint_indices[k] = weights_tri[i].bone_indices[k];
            v.joint_weights[k] = weights_tri[i].weights[k];
        }
    }

    // ── Bucket triangles by material slot (geometry-local) ───
    const size_t tri_count = tri->indices.size() / 3;
    std::unordered_map<int32_t, std::vector<uint32_t>> by_slot;
    for (size_t t = 0; t < tri_count; ++t) {
        int32_t slot = -1;
        if (t < tri->material_per_tri.size()) slot = tri->material_per_tri[t];
        auto& buf = by_slot[slot];
        buf.push_back(vertex_base + static_cast<uint32_t>(tri->indices[t*3 + 0]));
        buf.push_back(vertex_base + static_cast<uint32_t>(tri->indices[t*3 + 1]));
        buf.push_back(vertex_base + static_cast<uint32_t>(tri->indices[t*3 + 2]));
    }
    for (auto& [slot, inds] : by_slot) {
        SubMesh sm;
        sm.index_start = static_cast<uint32_t>(out.indices.size());
        sm.index_count = static_cast<uint32_t>(inds.size());
        sm.material_id = (slot >= 0 && static_cast<size_t>(slot) < slot_mats.size())
                         ? slot_mats[slot] : 0;
        sm.debug_name  = dbg_name;
        out.indices.insert(out.indices.end(), inds.begin(), inds.end());
        out.submeshes.push_back(std::move(sm));
    }
}

PackedMesh pack_mesh_from_fbx(const FBXImportResult& r) {
    PackedMesh out;
    if (!r.scene) return out;

    // Pack every triangulated geometry in the scene.
    for (FBXObjectId gid : r.scene->ids_of_type(FBXObjectType::GEOMETRY)) {
        pack_one_geometry(*r.scene, gid, r.skeleton, out);
    }
    if (out.vertices.empty()) return out;

    // Bounding sphere over the combined positions.
    float3 mn{out.vertices[0].position[0], out.vertices[0].position[1], out.vertices[0].position[2]};
    float3 mx = mn;
    for (const auto& v : out.vertices) {
        mn = {std::min(mn.x, v.position[0]), std::min(mn.y, v.position[1]), std::min(mn.z, v.position[2])};
        mx = {std::max(mx.x, v.position[0]), std::max(mx.y, v.position[1]), std::max(mx.z, v.position[2])};
    }
    out.center = {(mn.x+mx.x)*0.5f, (mn.y+mx.y)*0.5f, (mn.z+mx.z)*0.5f};
    float dx = mx.x-mn.x, dy = mx.y-mn.y, dz = mx.z-mn.z;
    out.radius = 0.5f * std::sqrt(dx*dx + dy*dy + dz*dz);
    if (out.radius < 0.01f) out.radius = 1.0f;
    return out;
}

/// Walk each submesh's material → diffuse texture reference and resolve
/// to a bare filename (basename only). Called once after packing so the
/// cache path no longer needs an FBXScene.
static void resolve_submesh_textures(const FBXScene& scene, PackedMesh& mesh) {
    for (SubMesh& sm : mesh.submeshes) {
        sm.texture_basename.clear();
        if (sm.material_id == 0) continue;
        const FBXObject* mo = scene.get(sm.material_id);
        if (!mo || !mo->material) continue;

        FBXObjectId tex_id = 0;
        auto it = mo->material->textures.find("DiffuseColor");
        if (it != mo->material->textures.end()) tex_id = it->second;
        if (!tex_id && !mo->material->textures.empty()) {
            tex_id = mo->material->textures.begin()->second;
        }
        if (!tex_id) continue;

        const FBXObject* to = scene.get(tex_id);
        if (!to || !to->texture) continue;
        const std::string& fname = !to->texture->relative_filename.empty()
                                 ? to->texture->relative_filename
                                 : to->texture->file_name;
        size_t sep = fname.find_last_of("/\\");
        sm.texture_basename = (sep == std::string::npos) ? fname : fname.substr(sep + 1);
    }
}

// ============================================================
// External animation loading — merge extra clips into skeleton order.
// ============================================================

/// Remap a clip's channel indices from the source skeleton to the base
/// skeleton's indexing by bone name. Channels referencing bones missing
/// from the base skeleton are dropped.
AnimationClipDescriptor remap_clip_to_base(const AnimationClipDescriptor& src,
                                           const SkeletonDescriptor&     src_skel,
                                           const SkeletonDescriptor&     base_skel)
{
    AnimationClipDescriptor dst;
    dst.name        = src.name;
    dst.duration    = src.duration;
    dst.wrap_mode   = src.wrap_mode;
    dst.sample_rate = src.sample_rate;

    std::unordered_map<std::string, uint32_t> base_by_name;
    for (size_t i = 0; i < base_skel.bones.size(); ++i)
        base_by_name[base_skel.bones[i].name] = static_cast<uint32_t>(i);

    for (const AnimationChannel& ch : src.channels) {
        if (ch.target_index >= src_skel.bones.size()) continue;
        const std::string& name = src_skel.bones[ch.target_index].name;
        auto it = base_by_name.find(name);
        if (it == base_by_name.end()) continue;
        AnimationChannel rc = ch;
        rc.target_index = it->second;
        dst.channels.push_back(std::move(rc));
    }
    return dst;
}

// ============================================================
// Binary cache — serialises the whole load pipeline output so
// subsequent runs skip FBX parsing + triangulation entirely.
// Invalidated if any source file (model + anim fbxs) changed mtime.
// ============================================================

namespace cache {

constexpr char     MAGIC[8] = {'P','F','B','X','V','0','0','7'};
constexpr uint32_t VERSION  = 7;

struct SourceEntry {
    std::string path;
    int64_t     mtime_ns = 0;
};

static int64_t file_mtime_ns(const fs::path& p) {
    std::error_code ec;
    auto t = fs::last_write_time(p, ec);
    if (ec) return 0;
    return t.time_since_epoch().count();
}

// ── Binary I/O helpers ──────────────────────────────────────

template <class T>
static void write_pod(FILE* f, const T& v) {
    std::fwrite(&v, sizeof(T), 1, f);
}
template <class T>
static bool read_pod(FILE* f, T& v) {
    return std::fread(&v, sizeof(T), 1, f) == 1;
}

static void write_str(FILE* f, const std::string& s) {
    uint32_t n = static_cast<uint32_t>(s.size());
    std::fwrite(&n, sizeof(n), 1, f);
    if (n) std::fwrite(s.data(), 1, n, f);
}
static bool read_str(FILE* f, std::string& s) {
    uint32_t n = 0;
    if (std::fread(&n, sizeof(n), 1, f) != 1) return false;
    s.resize(n);
    if (n && std::fread(s.data(), 1, n, f) != n) return false;
    return true;
}

template <class T>
static void write_vec_pod(FILE* f, const std::vector<T>& v) {
    uint32_t n = static_cast<uint32_t>(v.size());
    std::fwrite(&n, sizeof(n), 1, f);
    if (n) std::fwrite(v.data(), sizeof(T), n, f);
}
template <class T>
static bool read_vec_pod(FILE* f, std::vector<T>& v) {
    uint32_t n = 0;
    if (std::fread(&n, sizeof(n), 1, f) != 1) return false;
    v.resize(n);
    if (n && std::fread(v.data(), sizeof(T), n, f) != n) return false;
    return true;
}

// ── Write ──────────────────────────────────────────────────

static bool write(const fs::path& cache_path,
                  const PackedMesh& mesh,
                  const SkeletonDescriptor& skeleton,
                  const std::vector<AnimationClipDescriptor>& clips,
                  const std::vector<SourceEntry>& sources)
{
    std::error_code ec;
    fs::create_directories(cache_path.parent_path(), ec);
    FILE* f = std::fopen(cache_path.string().c_str(), "wb");
    if (!f) return false;

    std::fwrite(MAGIC, 1, 8, f);
    write_pod(f, VERSION);

    uint32_t src_n = static_cast<uint32_t>(sources.size());
    write_pod(f, src_n);
    for (const auto& e : sources) {
        write_str(f, e.path);
        write_pod(f, e.mtime_ns);
    }

    write_pod(f, mesh.center);
    write_pod(f, mesh.radius);
    write_vec_pod(f, mesh.vertices);
    write_vec_pod(f, mesh.indices);

    uint32_t sm_n = static_cast<uint32_t>(mesh.submeshes.size());
    write_pod(f, sm_n);
    for (const SubMesh& sm : mesh.submeshes) {
        write_pod(f, sm.index_start);
        write_pod(f, sm.index_count);
        write_str(f, sm.texture_basename);
        write_str(f, sm.debug_name);
    }

    write_str(f, skeleton.name);
    uint32_t bone_n = static_cast<uint32_t>(skeleton.bones.size());
    write_pod(f, bone_n);
    for (const Bone& b : skeleton.bones) {
        write_str(f, b.name);
        write_pod(f, b.parent_index);
        write_pod(f, b.bind_pose);
        write_pod(f, b.inverse_bind_matrix);
    }

    uint32_t clip_n = static_cast<uint32_t>(clips.size());
    write_pod(f, clip_n);
    for (const AnimationClipDescriptor& c : clips) {
        write_str(f, c.name);
        write_pod(f, c.duration);
        uint8_t wm = static_cast<uint8_t>(c.wrap_mode);
        write_pod(f, wm);
        write_pod(f, c.sample_rate);
        uint32_t ch_n = static_cast<uint32_t>(c.channels.size());
        write_pod(f, ch_n);
        for (const AnimationChannel& ch : c.channels) {
            write_pod(f, ch.target_index);
            uint8_t tgt = static_cast<uint8_t>(ch.target);
            uint8_t ipm = static_cast<uint8_t>(ch.interpolation);
            write_pod(f, tgt);
            write_pod(f, ipm);
            write_vec_pod(f, ch.keyframes);
        }
    }

    std::fclose(f);
    return true;
}

// ── Read ───────────────────────────────────────────────────

/// Try to read a cache file. Returns true only if:
///   * magic / version match,
///   * stored source list matches `required_sources` (same paths in same order),
///   * every source's mtime is <= cached mtime (no file edited since cache).
static bool read(const fs::path& cache_path,
                 const std::vector<SourceEntry>& required_sources,
                 PackedMesh& mesh,
                 SkeletonDescriptor& skeleton,
                 std::vector<AnimationClipDescriptor>& clips)
{
    std::error_code ec;
    if (!fs::exists(cache_path, ec)) return false;
    FILE* f = std::fopen(cache_path.string().c_str(), "rb");
    if (!f) return false;

    auto fail = [&]() { std::fclose(f); return false; };

    char magic[8];
    if (std::fread(magic, 1, 8, f) != 8) return fail();
    if (std::memcmp(magic, MAGIC, 8) != 0) return fail();
    uint32_t ver = 0;
    if (!read_pod(f, ver) || ver != VERSION) return fail();

    uint32_t src_n = 0;
    if (!read_pod(f, src_n)) return fail();
    if (src_n != required_sources.size()) return fail();
    for (uint32_t i = 0; i < src_n; ++i) {
        std::string p; int64_t mt = 0;
        if (!read_str(f, p) || !read_pod(f, mt)) return fail();
        if (p != required_sources[i].path) return fail();
        if (required_sources[i].mtime_ns > mt) return fail();
    }

    if (!read_pod(f, mesh.center)) return fail();
    if (!read_pod(f, mesh.radius)) return fail();
    if (!read_vec_pod(f, mesh.vertices)) return fail();
    if (!read_vec_pod(f, mesh.indices))  return fail();

    uint32_t sm_n = 0;
    if (!read_pod(f, sm_n)) return fail();
    mesh.submeshes.resize(sm_n);
    for (SubMesh& sm : mesh.submeshes) {
        if (!read_pod(f, sm.index_start)) return fail();
        if (!read_pod(f, sm.index_count)) return fail();
        if (!read_str(f, sm.texture_basename)) return fail();
        if (!read_str(f, sm.debug_name)) return fail();
        sm.material_id = 0;
    }

    if (!read_str(f, skeleton.name)) return fail();
    uint32_t bone_n = 0;
    if (!read_pod(f, bone_n)) return fail();
    skeleton.bones.resize(bone_n);
    for (Bone& b : skeleton.bones) {
        if (!read_str(f, b.name)) return fail();
        if (!read_pod(f, b.parent_index)) return fail();
        if (!read_pod(f, b.bind_pose)) return fail();
        if (!read_pod(f, b.inverse_bind_matrix)) return fail();
    }

    uint32_t clip_n = 0;
    if (!read_pod(f, clip_n)) return fail();
    clips.resize(clip_n);
    for (AnimationClipDescriptor& c : clips) {
        if (!read_str(f, c.name)) return fail();
        if (!read_pod(f, c.duration)) return fail();
        uint8_t wm = 0;
        if (!read_pod(f, wm)) return fail();
        c.wrap_mode = static_cast<WrapMode>(wm);
        if (!read_pod(f, c.sample_rate)) return fail();
        uint32_t ch_n = 0;
        if (!read_pod(f, ch_n)) return fail();
        c.channels.resize(ch_n);
        for (AnimationChannel& ch : c.channels) {
            if (!read_pod(f, ch.target_index)) return fail();
            uint8_t tgt = 0, ipm = 0;
            if (!read_pod(f, tgt)) return fail();
            if (!read_pod(f, ipm)) return fail();
            ch.target        = static_cast<ChannelTarget>(tgt);
            ch.interpolation = static_cast<InterpolationMode>(ipm);
            if (!read_vec_pod(f, ch.keyframes)) return fail();
        }
    }

    std::fclose(f);
    return true;
}

} // namespace cache

// ============================================================
// Shader / buffer layouts (CPU side; must match model.vert)
// ============================================================

struct SceneUBO {
    float view[16];
    float proj[16];
    float light_dir[4];
    float light_color[4];
    float camera_pos[4];
};

struct InstanceData {
    float    model[16];
    float    base_color[4];
    uint32_t skin_info[4];
};

constexpr uint32_t kMaxBones = 256;

// ============================================================
// Vulkan helpers
// ============================================================

// find_memory_type / create_buffer / load_shader_spv: see vk_buffer_util.h

// ============================================================
// GPUTexture — image + memory + view + sampler bundle.
// ============================================================

struct GPUTexture {
    VkImage        image   = VK_NULL_HANDLE;
    VkDeviceMemory memory  = VK_NULL_HANDLE;
    VkImageView    view    = VK_NULL_HANDLE;
    VkSampler      sampler = VK_NULL_HANDLE;
    uint32_t       width = 0, height = 0;
};

/// Single-submit command buffer helper.
static VkCommandBuffer begin_one_shot(VkDevice d, VkCommandPool pool) {
    VkCommandBufferAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ai.commandPool = pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(d, &ai, &cb);
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cb, &bi);
    return cb;
}

static void end_one_shot(VkDevice d, VkQueue q, VkCommandPool pool, VkCommandBuffer cb) {
    vkEndCommandBuffer(cb);
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    vkQueueSubmit(q, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(q);
    vkFreeCommandBuffers(d, pool, 1, &cb);
}

static bool upload_texture_rgba8(VkDevice d, VkPhysicalDevice pd,
                                 VkCommandPool pool, VkQueue q,
                                 const uint8_t* pixels, uint32_t w, uint32_t h,
                                 GPUTexture& out) {
    const VkDeviceSize size = static_cast<VkDeviceSize>(w) * h * 4;

    VkBuffer staging; VkDeviceMemory staging_mem;
    if (!create_buffer(d, pd, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                       staging, staging_mem)) return false;
    void* map = nullptr;
    vkMapMemory(d, staging_mem, 0, size, 0, &map);
    std::memcpy(map, pixels, size);
    vkUnmapMemory(d, staging_mem);

    VkImageCreateInfo ii{};
    ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format    = VK_FORMAT_R8G8B8A8_SRGB;
    ii.extent    = {w, h, 1};
    ii.mipLevels = 1; ii.arrayLayers = 1;
    ii.samples   = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling    = VK_IMAGE_TILING_OPTIMAL;
    ii.usage     = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(d, &ii, nullptr, &out.image) != VK_SUCCESS) {
        vkDestroyBuffer(d, staging, nullptr); vkFreeMemory(d, staging_mem, nullptr);
        return false;
    }
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(d, out.image, &req);
    VkMemoryAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize  = req.size;
    ai.memoryTypeIndex = find_memory_type(pd, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (ai.memoryTypeIndex == UINT32_MAX ||
        vkAllocateMemory(d, &ai, nullptr, &out.memory) != VK_SUCCESS) {
        vkDestroyBuffer(d, staging, nullptr); vkFreeMemory(d, staging_mem, nullptr);
        return false;
    }
    vkBindImageMemory(d, out.image, out.memory, 0);

    // Transition UNDEFINED -> TRANSFER_DST, copy, then -> SHADER_READ_ONLY.
    VkCommandBuffer cb = begin_one_shot(d, pool);
    auto barrier = [&](VkImageLayout from, VkImageLayout to,
                       VkAccessFlags srcA, VkAccessFlags dstA,
                       VkPipelineStageFlags srcS, VkPipelineStageFlags dstS) {
        VkImageMemoryBarrier b{};
        b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.oldLayout = from; b.newLayout = to;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = out.image;
        b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        b.subresourceRange.levelCount = 1;
        b.subresourceRange.layerCount = 1;
        b.srcAccessMask = srcA; b.dstAccessMask = dstA;
        vkCmdPipelineBarrier(cb, srcS, dstS, 0, 0, nullptr, 0, nullptr, 1, &b);
    };
    barrier(VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            0, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

    VkBufferImageCopy region{};
    region.bufferOffset = 0;
    region.bufferRowLength = 0; region.bufferImageHeight = 0;
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = {w, h, 1};
    vkCmdCopyBufferToImage(cb, staging, out.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    barrier(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    end_one_shot(d, q, pool, cb);

    vkDestroyBuffer(d, staging, nullptr);
    vkFreeMemory(d, staging_mem, nullptr);

    VkImageViewCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = out.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format   = VK_FORMAT_R8G8B8A8_SRGB;
    vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vi.subresourceRange.levelCount = 1;
    vi.subresourceRange.layerCount = 1;
    if (vkCreateImageView(d, &vi, nullptr, &out.view) != VK_SUCCESS) return false;

    VkSamplerCreateInfo sc{};
    sc.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sc.magFilter = sc.minFilter = VK_FILTER_LINEAR;
    sc.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sc.addressModeU = sc.addressModeV = sc.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sc.maxLod = 1.0f;
    sc.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
    if (vkCreateSampler(d, &sc, nullptr, &out.sampler) != VK_SUCCESS) return false;

    out.width = w; out.height = h;
    return true;
}

static void destroy_texture(VkDevice d, GPUTexture& t) {
    if (t.sampler) { vkDestroySampler(d, t.sampler, nullptr); t.sampler = VK_NULL_HANDLE; }
    if (t.view)    { vkDestroyImageView(d, t.view,   nullptr); t.view    = VK_NULL_HANDLE; }
    if (t.image)   { vkDestroyImage(d, t.image,      nullptr); t.image   = VK_NULL_HANDLE; }
    if (t.memory)  { vkFreeMemory(d, t.memory,       nullptr); t.memory  = VK_NULL_HANDLE; }
}

/// Find a texture file under `texture_dir` whose stem matches `stem`
/// (case-insensitive, any supported extension).
static std::string find_texture_file(const fs::path& texture_dir, const std::string& filename) {
    if (!fs::exists(texture_dir)) return {};
    fs::path want_stem = fs::path(filename).stem();
    std::string want_lower = want_stem.string();
    std::transform(want_lower.begin(), want_lower.end(), want_lower.begin(),
                   [](unsigned char c){ return static_cast<char>(std::tolower(c)); });

    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(texture_dir, ec)) {
        if (!entry.is_regular_file()) continue;
        std::string stem = entry.path().stem().string();
        std::transform(stem.begin(), stem.end(), stem.begin(),
                       [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
        if (stem == want_lower) return entry.path().string();
    }
    return {};
}

} // namespace

// ============================================================
// Viewer options (CLI)
// ============================================================

struct ViewerOptions {
    bool           fur_enabled     = false;
    FurShellParams fur;
    bool           fur_length_set  = false;   // --fur-length given (else radius-relative default)
    std::string    capture_path;              // --capture <file.bmp>
    uint32_t       capture_frame   = 90;      // --capture-frame N
    bool           exit_after_capture = true; // --stay keeps the window open
    bool           show_bones      = true;    // --no-bones hides the skeleton overlay
    bool           bind_enabled    = false;   // --bind [pattern]
    BindingPatternKind bind_pattern = BindingPatternKind::Obi;
    float          bind_tightness  = 1.0f;    // --tightness X (0..1)
    bool           bind_wrap_anim  = false;   // --bind-anim: rope wraps + tightens over time
    bool           show_rope       = true;    // --no-rope: deformation only (debug the dent)
    bool           rope_tail       = true;    // --no-rope-tail: no loose end toward the camera
    bool           tears           = false;   // --tears: toon tears from the detected eyes
};

namespace {

/// @implements SPEC-FBX-VIEWER-FUR-EFFECTS
bool parse_uint_option(const char* text, uint32_t min_value, uint32_t max_value,
                       uint32_t& out) {
    if (!text || *text == '\0' || *text == '-') return false;
    errno = 0;
    char* end = nullptr;
    const unsigned long value = std::strtoul(text, &end, 10);
    if (errno == ERANGE || !end || *end != '\0' || value < min_value || value > max_value) {
        return false;
    }
    out = static_cast<uint32_t>(value);
    return true;
}

/// @implements SPEC-FBX-VIEWER-FUR-EFFECTS
bool parse_float_option(const char* text, float min_value, float max_value, float& out) {
    if (!text || *text == '\0') return false;
    errno = 0;
    char* end = nullptr;
    const float value = std::strtof(text, &end);
    if (errno == ERANGE || !end || *end != '\0' || !std::isfinite(value) ||
        value < min_value || value > max_value) {
        return false;
    }
    out = value;
    return true;
}

} // namespace

// ============================================================
// FBXViewer
// ============================================================

class FBXViewer {
public:
    bool initialize(PackedMesh mesh,
                    SkeletonDescriptor skeleton,
                    std::vector<AnimationClipDescriptor> clips,
                    const fs::path& model_dir,
                    const std::string& shader_dir,
                    const ViewerOptions& options) {
        mesh_       = std::move(mesh);
        skeleton_   = std::move(skeleton);
        clips_      = std::move(clips);
        model_dir_  = model_dir;
        shader_dir_ = shader_dir;
        options_    = options;
        fur_enabled_ = options.fur_enabled;
        bind_enabled_ = options.bind_enabled;
        bind_pattern_ = options.bind_pattern;
        bind_tightness_ = options.bind_tightness;
        show_rope_ = options.show_rope;
        rope_tail_ = options.rope_tail;
        tears_enabled_ = options.tears;
        fur_ = options.fur;
        if (!options.fur_length_set) {
            // Reference plush is short-pile minky: the nap is a fuzzy skin,
            // not visible strands, so keep the extrusion under 0.5% of the
            // bounding radius (the FBX may be in cm, hence radius-relative).
            fur_.length  = mesh_.radius * 0.0045f;
            fur_.bend[1] = -mesh_.radius * 0.001f;
        }
        show_bones_ = options.show_bones;
        if (!options.capture_path.empty()) {
            capture_.request(options.capture_path, options.capture_frame);
        }

        GlfwWindowConfig wc; wc.width = 1280; wc.height = 720; wc.title = "Pictor FBX Viewer";
        if (!provider_.create(wc)) { std::fprintf(stderr, "GLFW window create failed\n"); return false; }
        VulkanContextConfig vcfg;
        vcfg.app_name = "pictor_fbx_viewer";
        // This demo updates one shared set of host-visible resources. Keep it
        // serial rather than allowing a later flight to overwrite GPU input.
        vcfg.frames_in_flight = 1;
        vcfg.enable_swapchain_transfer_src = !options.capture_path.empty();
        if (!vk_.initialize(&provider_, vcfg)) { std::fprintf(stderr, "Vulkan init failed\n"); return false; }
        if (!options.capture_path.empty() && !FrameCapture::supports_format(vk_.swapchain_format())) {
            std::fprintf(stderr, "Capture requires an RGBA8 or BGRA8 swapchain format\n");
            return false;
        }

        // Keyboard callback needs access to this.
        glfwSetWindowUserPointer(provider_.glfw_window(), this);
        glfwSetKeyCallback(provider_.glfw_window(), &FBXViewer::key_callback);

        if (!create_depth_resources())       return false;
        if (!create_render_pass())           return false;
        if (!create_framebuffers())          return false;
        if (!create_descriptor_layouts())    return false;
        if (!create_pipeline())              return false;
        if (!create_bone_pipeline())         return false;
        // Optional effects must not make the legacy viewer depend on their
        // shaders. Keyboard toggles create these pipelines lazily.
        if (fur_enabled_ && !create_fur_pass()) return false;
        if (bind_enabled_ && show_rope_ && !create_rope_pass()) return false;
        if (tears_enabled_ && !create_tear_pass()) return false;
        if (!create_buffers())               return false;
        if (!load_textures())                return false;
        if (!create_descriptor_sets())       return false;

        AnimationSystemConfig acfg;
        acfg.gpu_skinning_enabled   = false;
        acfg.max_bones_per_skeleton = kMaxBones;
        acfg.max_active_instances   = 8;
        anim_.initialize(acfg);

        if (!skeleton_.bones.empty()) {
            skel_ = anim_.register_skeleton(skeleton_);
            for (const auto& c : clips_) clip_handles_.push_back(anim_.register_clip(c));
            inst_ = anim_.create_instance(42, skel_);
            if (!clip_handles_.empty()) {
                anim_.play(inst_, clip_handles_[clip_index_], 1.0f, 1.0f);
                std::printf("Playing clip [%zu/%zu]: %s\n", clip_index_ + 1,
                            clips_.size(), clips_[clip_index_].name.c_str());
            }
        }
        std::printf("Skinning: ON (press B to toggle bind-pose).\n");
        std::printf("Fur shells: %s (press F to toggle).\n", fur_enabled_ ? "ON" : "OFF");
        fur_.print();

        if (!setup_binding()) return false;
        std::printf("Rope binding: %s (press T to toggle, Y pattern, I/K tightness).\n",
                    bind_enabled_ ? "ON" : "OFF");
        std::printf("Tears: %s (%zu eye anchor%s, press J to toggle).\n",
                    tears_enabled_ ? "ON" : "OFF", eye_anchors_.size(), eye_anchors_.size() == 1 ? "" : "s");
        return true;
    }

    // ─── toon tears ──────────────────────────────────────────
    bool create_tear_pass() {
        if (tear_pass_.valid()) return true;
        TearPass::CreateInfo ci;
        ci.device          = vk_.device();
        ci.physical_device = vk_.physical_device();
        ci.render_pass     = render_pass_;
        ci.scene_layout    = scene_set_layout_;
        ci.shader_dir      = shader_dir_;
        if (!tear_pass_.create(ci)) {
            std::fprintf(stderr, "Tear pipeline unavailable (missing tear.*.spv?)\n");
            return false;
        }
        return true;
    }

    /// Current-pose position/normal of a packed vertex (CPU skinning with
    /// the same matrices the shader uses; identity when skinning is off).
    void skinned_vertex(uint32_t index, float3& pos_out, float3& normal_out) const {
        const TexturedSkinnedVertex& v = mesh_.vertices[index];
        pos_out    = {v.position[0], v.position[1], v.position[2]};
        normal_out = {v.normal[0], v.normal[1], v.normal[2]};
        if (!skinning_enabled_ || inst_ == INVALID_ANIMATION_STATE) return;
        const float4x4* sk = anim_.get_skinning_matrices(inst_);
        const uint32_t bone_count = anim_.get_bone_count(inst_);
        if (!sk) return;
        float w_sum = v.joint_weights[0] + v.joint_weights[1] + v.joint_weights[2] + v.joint_weights[3];
        if (w_sum < 1e-4f) return;
        float3 p{0, 0, 0}, n{0, 0, 0};
        for (int j = 0; j < 4; ++j) {
            const float w = v.joint_weights[j];
            if (w <= 0.0f || v.joint_indices[j] >= bone_count) continue;
            const float4x4& m = sk[v.joint_indices[j]];
            // Row-vector convention: p' = p * M (translation in m.m[3][*]).
            p.x += w * (pos_out.x * m.m[0][0] + pos_out.y * m.m[1][0] + pos_out.z * m.m[2][0] + m.m[3][0]);
            p.y += w * (pos_out.x * m.m[0][1] + pos_out.y * m.m[1][1] + pos_out.z * m.m[2][1] + m.m[3][1]);
            p.z += w * (pos_out.x * m.m[0][2] + pos_out.y * m.m[1][2] + pos_out.z * m.m[2][2] + m.m[3][2]);
            n.x += w * (normal_out.x * m.m[0][0] + normal_out.y * m.m[1][0] + normal_out.z * m.m[2][0]);
            n.y += w * (normal_out.x * m.m[0][1] + normal_out.y * m.m[1][1] + normal_out.z * m.m[2][1]);
            n.z += w * (normal_out.x * m.m[0][2] + normal_out.y * m.m[1][2] + normal_out.z * m.m[2][2]);
        }
        pos_out = p;
        float l = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
        normal_out = l > 1e-6f ? float3{n.x / l, n.y / l, n.z / l} : normal_out;
    }

    /// Project a point onto the bind-pose body surface using the binding
    /// frame's raycast radius (drops hug the belly instead of entering it).
    bool project_to_body(const float3& p, float3& on_surface, float3& normal) const {
        if (!bind_surface_radius_) return false;
        const float3 a = bind_frame_.start, b = bind_frame_.end;
        const float3 axis{b.x - a.x, b.y - a.y, b.z - a.z};
        const float len2 = axis.x * axis.x + axis.y * axis.y + axis.z * axis.z;
        if (len2 < 1e-6f) return false;
        const float3 rel{p.x - a.x, p.y - a.y, p.z - a.z};
        const float t = (rel.x * axis.x + rel.y * axis.y + rel.z * axis.z) / len2;
        if (t < -0.15f || t > 1.15f) return false;
        const float3 axis_pt{a.x + axis.x * t, a.y + axis.y * t, a.z + axis.z * t};
        float3 radial{p.x - axis_pt.x, p.y - axis_pt.y, p.z - axis_pt.z};
        // Remove the axial component so the direction is purely radial.
        const float along = (radial.x * axis.x + radial.y * axis.y + radial.z * axis.z) / len2;
        radial = {radial.x - axis.x * along, radial.y - axis.y * along, radial.z - axis.z * along};
        const float rl = std::sqrt(radial.x * radial.x + radial.y * radial.y + radial.z * radial.z);
        if (rl < 1e-4f) return false;
        radial = {radial.x / rl, radial.y / rl, radial.z / rl};
        // theta relative to the frame's theta-zero direction (z axis by construction).
        const float theta = std::atan2(radial.x, radial.z);
        const float radius = bind_surface_radius_(std::max(0.0f, std::min(1.0f, t)), theta);
        on_surface = {axis_pt.x + radial.x * radius, axis_pt.y + radial.y * radius, axis_pt.z + radial.z * radius};
        normal = radial;
        return true;
    }

    void update_tears(float elapsed) {
        if (!tear_pass_.valid()) return;
        if (!tears_enabled_ || eye_anchors_.empty()) { tear_pass_.update(elapsed, {}, camera_pos_, nullptr, tear_params_); return; }
        std::vector<TearEmitter> emitters;
        for (const EyeAnchor& a : eye_anchors_) {
            TearEmitter e;
            skinned_vertex(a.vertex_index, e.position, e.normal);
            emitters.push_back(e);
        }
        // Outward = away from the other eye (the fan sprays from the outer corner).
        for (size_t i = 0; i < emitters.size(); ++i) {
            float3 o{0, 0, 0};
            for (size_t j = 0; j < emitters.size(); ++j) {
                if (j == i) continue;
                o = {o.x + emitters[i].position.x - emitters[j].position.x,
                     o.y + emitters[i].position.y - emitters[j].position.y,
                     o.z + emitters[i].position.z - emitters[j].position.z};
            }
            if (o.x * o.x + o.y * o.y + o.z * o.z < 1e-8f) {
                // Single eye: spray toward camera-right.
                const float3 v{camera_pos_.x - emitters[i].position.x, 0.0f, camera_pos_.z - emitters[i].position.z};
                o = {v.z, 0.0f, -v.x};
            }
            emitters[i].outward = o;
        }
        tear_params_.drop_size    = mesh_.radius * 0.036f;
        tear_params_.line_start   = mesh_.radius * 0.01f;    // first drop touching the eye corner
        tear_params_.line_length  = mesh_.radius * 0.24f;    // wider gaps between the three drops
        // Lift well clear of the fur and the paws that protrude beside the face.
        tear_params_.surface_lift = fur_.length + mesh_.radius * 0.04f;
        tear_pass_.update(elapsed, emitters, camera_pos_,
                          [this](const float3& p, float3& s, float3& n) { return project_to_body(p, s, n); },
                          tear_params_);
    }

    /// Loose end of the rope: leaves the middle loop at the point facing
    /// the camera and trails toward the viewer with a little sag.
    void update_rope_tail() {
        if (!rope_pass_.valid()) return;
        std::vector<float3> tail;
        if (bind_enabled_ && show_rope_ && rope_tail_ && !bind_chain_.strands.empty()) {
            const auto& strand = bind_chain_.strands[bind_chain_.strands.size() / 2];
            const float3 c{0.5f * (bind_frame_.start.x + bind_frame_.end.x),
                           0.5f * (bind_frame_.start.y + bind_frame_.end.y),
                           0.5f * (bind_frame_.start.z + bind_frame_.end.z)};
            const float3 to_cam{camera_pos_.x - c.x, 0.0f, camera_pos_.z - c.z};
            float best = -2.0f; float3 start = strand.front();
            for (const float3& p : strand) {
                const float3 r{p.x - c.x, 0.0f, p.z - c.z};
                const float rl = std::sqrt(r.x * r.x + r.z * r.z), cl = std::sqrt(to_cam.x * to_cam.x + to_cam.z * to_cam.z);
                if (rl < 1e-4f || cl < 1e-4f) continue;
                const float d = (r.x * to_cam.x + r.z * to_cam.z) / (rl * cl);
                if (d > best) { best = d; start = p; }
            }
            float3 dir{camera_pos_.x - start.x, camera_pos_.y - start.y, camera_pos_.z - start.z};
            const float dist = std::sqrt(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
            if (dist > 1e-3f) {
                dir = {dir.x / dist, dir.y / dist, dir.z / dist};
                const float length = std::min(dist * 0.55f, mesh_.radius * 2.6f);
                const float sag = length * 0.22f;
                const int n = 18;
                for (int i = 0; i <= n; ++i) {
                    const float s = static_cast<float>(i) / n;
                    // Start slightly inside the loop so the joint is hidden.
                    const float along = -bind_profile_.rope_radius * 0.8f + length * s;
                    tail.push_back({start.x + dir.x * along,
                                    start.y + dir.y * along - sag * s * s,
                                    start.z + dir.z * along});
                }
            }
        }
        rope_pass_.update_tail(tail, bind_profile_.rope_radius);
    }

    // ─── rope binding ────────────────────────────────────────
    //
    // The bind frame is estimated from the bind-pose vertex cloud; the
    // deformation profile is expressed in metres and converted to model
    // units by treating the reference plush as roughly 0.5 m tall.
    bool setup_binding() {
        std::vector<float3> positions;
        positions.reserve(mesh_.vertices.size());
        for (const auto& v : mesh_.vertices)
            positions.push_back({v.position[0], v.position[1], v.position[2]});
        bind_frame_ = estimate_binding_frame(positions.data(), positions.size(), 0.20f, 0.70f);
        // The frame is only the axis; the rope hugs the real body surface.
        bind_surface_radius_ = make_mesh_surface_radius(positions.data(), positions.size(),
                                                        mesh_.indices.data(), mesh_.indices.size(),
                                                        bind_frame_);
        float y_min = positions[0].y, y_max = positions[0].y;
        for (const float3& p : positions) { y_min = std::min(y_min, p.y); y_max = std::max(y_max, p.y); }
        const float metres_to_units = std::max(y_max - y_min, 1e-3f) / 0.5f;
        bind_base_profile_ = BindingBodyProfile::scaled_plush(metres_to_units);
        if (!rebuild_binding_chain()) return false;
        std::printf("[bind] frame y=%.2f..%.2f radius=%.2f..%.2f rope_r=%.3f sink=%.3f groove=%.3f\n",
                    bind_frame_.start.y, bind_frame_.end.y,
                    bind_frame_.radius_start, bind_frame_.radius_end,
                    bind_profile_.rope_radius, bind_profile_.sink_depth, bind_profile_.groove_width);
        return true;
    }

    bool rebuild_binding_chain() {
        BindingBodyProfile replacement_profile = bind_base_profile_.pinched_for(bind_pattern_);
        BindingPatternSpec spec = BindingPatternSpec::create_default(bind_pattern_)
                                      .limit_turns_for(bind_frame_, replacement_profile);
        NawaCapsuleChain replacement_chain = build_capsule_chain(
            spec, bind_frame_, replacement_profile, bind_surface_radius_);
        const uint32_t replacement_visible =
            static_cast<uint32_t>(replacement_chain.segments.size());
        if (rope_pass_.valid()) {
            if (!rope_pass_.build_geometry(replacement_chain, replacement_profile.rope_radius,
                                           replacement_visible)) {
                std::fprintf(stderr, "[bind] failed to build rope geometry\n");
                return false;
            }
        }
        bind_profile_ = replacement_profile;
        bind_chain_ = std::move(replacement_chain);
        bind_visible_segments_ = replacement_visible;
        std::printf("[bind] pattern=%s turns=%.1f segments=%zu tightness=%.2f\n",
                    binding_pattern_name(bind_pattern_), spec.turns, bind_chain_.segments.size(), bind_tightness_);
        return true;
    }

    /// Wrap, then tighten with a short ease-in.
    void update_binding(float elapsed) {
        float tightness = bind_tightness_;
        uint32_t visible = static_cast<uint32_t>(bind_chain_.segments.size());
        if (options_.bind_wrap_anim) {
            const float wrap_s = 0.9f, tighten_s = 0.35f;
            float wrap = std::min(1.0f, elapsed / wrap_s);
            float te = std::max(0.0f, elapsed - wrap_s);
            float t  = std::min(1.0f, te / tighten_s);
            tightness = bind_tightness_ * (t * t * (3.0f - 2.0f * t));
            visible = static_cast<uint32_t>(std::ceil(bind_chain_.segments.size() * wrap));
            if (visible != bind_visible_segments_) {
                bind_visible_segments_ = visible;
                if (rope_pass_.valid()) {
                    if (!rope_pass_.build_geometry(bind_chain_, bind_profile_.rope_radius, visible)) {
                        std::fprintf(stderr, "[bind] failed to update rope wrap geometry\n");
                    }
                }
            }
        }
        NawaBindUBO ubo = bind_enabled_
            ? compose_bind_ubo(bind_chain_, bind_profile_, tightness, visible)
            : NawaBindUBO{};
        void* p = nullptr;
        if (vkMapMemory(vk_.device(), bind_mem_, 0, sizeof(NawaBindUBO), 0, &p) != VK_SUCCESS) {
            std::fprintf(stderr, "[bind] failed to map uniform buffer\n");
            return;
        }
        std::memcpy(p, &ubo, sizeof(NawaBindUBO));
        vkUnmapMemory(vk_.device(), bind_mem_);
    }

    bool on_bind_key(int key) {
        switch (key) {
            case GLFW_KEY_T:
                if (!bind_enabled_ && show_rope_ && !rope_pass_.valid()) {
                    if (!create_rope_pass()) return true;
                    if (!rope_pass_.build_geometry(bind_chain_, bind_profile_.rope_radius,
                                                   bind_visible_segments_)) {
                        std::fprintf(stderr, "[bind] failed to build rope geometry\n");
                        return true;
                    }
                }
                bind_enabled_ = !bind_enabled_;
                std::printf("Rope binding: %s\n", bind_enabled_ ? "ON" : "OFF");
                return true;
            case GLFW_KEY_Y: {
                const BindingPatternKind previous_pattern = bind_pattern_;
                bind_pattern_ = static_cast<BindingPatternKind>(
                    (static_cast<uint32_t>(bind_pattern_) + 1) % 4);
                if (!rebuild_binding_chain()) bind_pattern_ = previous_pattern;
                return true;
            }
            case GLFW_KEY_H:
                if (!show_rope_ && bind_enabled_ && !rope_pass_.valid()) {
                    if (!create_rope_pass()) return true;
                    if (!rope_pass_.build_geometry(bind_chain_, bind_profile_.rope_radius,
                                                   bind_visible_segments_)) {
                        std::fprintf(stderr, "[bind] failed to build rope geometry\n");
                        return true;
                    }
                }
                show_rope_ = !show_rope_;
                std::printf("Rope geometry: %s\n", show_rope_ ? "ON" : "OFF");
                return true;
            case GLFW_KEY_J:
                if (!tears_enabled_ && !tear_pass_.valid() && !create_tear_pass()) return true;
                tears_enabled_ = !tears_enabled_;
                std::printf("Tears: %s\n", tears_enabled_ ? "ON" : "OFF");
                return true;
            case GLFW_KEY_I: bind_tightness_ = std::max(0.0f, bind_tightness_ - 0.1f); break;
            case GLFW_KEY_K: bind_tightness_ = std::min(1.0f, bind_tightness_ + 0.1f); break;
            default: return false;
        }
        std::printf("[bind] tightness=%.2f\n", bind_tightness_);
        return true;
    }

    void run() {
        auto t0 = std::chrono::steady_clock::now();
        auto prev = t0;
        while (!provider_.should_close()) {
            provider_.poll_events();
            auto now = std::chrono::steady_clock::now();
            float dt = std::chrono::duration<float>(now - prev).count();
            float elapsed = std::chrono::duration<float>(now - t0).count();
            prev = now;

            if (skinning_enabled_) anim_.update(dt);

            uint32_t img_idx = vk_.acquire_next_image();
            if (img_idx == UINT32_MAX) { handle_resize(); continue; }
            update_uniforms(elapsed);
            update_binding(elapsed);
            update_rope_tail();
            update_tears(elapsed);
            update_debug_bones();
            record_and_submit(img_idx);
            vk_.present(img_idx);
            if (capture_pending_) {
                capture_pending_ = false;
                capture_.finish(vk_.device(), vk_.graphics_queue(), vk_.swapchain_format());
                if (options_.exit_after_capture) {
                    glfwSetWindowShouldClose(provider_.glfw_window(), GLFW_TRUE);
                }
            }
            ++frame_index_;
        }
        vkDeviceWaitIdle(vk_.device());
    }

    void shutdown() {
        VkDevice d = vk_.device();
        if (d) vkDeviceWaitIdle(d);
        if (inst_ != INVALID_ANIMATION_STATE) anim_.destroy_instance(inst_);
        anim_.shutdown();

        auto safe_buf = [&](VkBuffer& b, VkDeviceMemory& m) {
            if (b) { vkDestroyBuffer(d, b, nullptr); b = VK_NULL_HANDLE; }
            if (m) { vkFreeMemory(d, m, nullptr);    m = VK_NULL_HANDLE; }
        };
        safe_buf(ubo_buffer_, ubo_mem_);
        safe_buf(instance_buffer_, instance_mem_);
        safe_buf(bone_buffer_, bone_mem_);
        safe_buf(vb_, vb_mem_);
        safe_buf(ib_, ib_mem_);
        safe_buf(debug_vb_, debug_vb_mem_);

        for (auto& [_, tex] : textures_) destroy_texture(d, tex);
        textures_.clear();
        destroy_texture(d, fallback_texture_);

        fur_pass_.destroy();
        rope_pass_.destroy();
        tear_pass_.destroy();
        safe_buf(bind_buffer_, bind_mem_);
        capture_.destroy(d);
        if (pipeline_)              vkDestroyPipeline(d, pipeline_, nullptr);
        if (pipeline_layout_)       vkDestroyPipelineLayout(d, pipeline_layout_, nullptr);
        if (debug_pipeline_)        vkDestroyPipeline(d, debug_pipeline_, nullptr);
        if (debug_pipeline_layout_) vkDestroyPipelineLayout(d, debug_pipeline_layout_, nullptr);
        if (scene_set_layout_)      vkDestroyDescriptorSetLayout(d, scene_set_layout_, nullptr);
        if (tex_set_layout_)        vkDestroyDescriptorSetLayout(d, tex_set_layout_, nullptr);
        if (desc_pool_)             vkDestroyDescriptorPool(d, desc_pool_, nullptr);

        for (auto fb : framebuffers_) if (fb) vkDestroyFramebuffer(d, fb, nullptr);
        framebuffers_.clear();
        if (render_pass_) vkDestroyRenderPass(d, render_pass_, nullptr);
        destroy_depth_resources();

        vk_.shutdown();
    }

private:
    static void key_callback(GLFWwindow* w, int key, int /*sc*/, int action, int /*mods*/) {
        if (action != GLFW_PRESS) return;
        auto* self = static_cast<FBXViewer*>(glfwGetWindowUserPointer(w));
        if (!self) return;
        self->on_key(key);
    }

    void on_key(int key) {
        if (key == GLFW_KEY_B) {
            skinning_enabled_ = !skinning_enabled_;
            std::printf("Skinning: %s\n", skinning_enabled_ ? "ON" : "OFF (bind pose)");
            return;
        }
        if (key == GLFW_KEY_L) {
            show_bones_ = !show_bones_;
            std::printf("Bone overlay: %s\n", show_bones_ ? "ON" : "OFF");
            return;
        }
        if (key == GLFW_KEY_M) {
            show_mesh_ = !show_mesh_;
            std::printf("Mesh: %s\n", show_mesh_ ? "ON" : "OFF");
            return;
        }
        if (on_fur_key(key)) return;
        if (on_bind_key(key)) return;
        if (clip_handles_.empty()) return;
        size_t n = clip_handles_.size();
        size_t prev = clip_index_;
        switch (key) {
            case GLFW_KEY_SPACE:
            case GLFW_KEY_N:  clip_index_ = (clip_index_ + 1) % n; break;
            case GLFW_KEY_P:  clip_index_ = (clip_index_ + n - 1) % n; break;
            case GLFW_KEY_R:  /* restart by re-playing */              break;
            default: return;
        }
        anim_.play(inst_, clip_handles_[clip_index_], 1.0f, 1.0f);
        std::printf("Playing clip [%zu/%zu]: %s%s\n",
                    clip_index_ + 1, n,
                    clips_[clip_index_].name.c_str(),
                    (prev == clip_index_ && key == GLFW_KEY_R) ? " (restart)" : "");
    }

    /// Fur shell tuning keys. Returns true when the key was consumed.
    ///   F        toggle shells
    ///   [ / ]    shell count -/+
    ///   - / =    fur length x0.8 / x1.25
    ///   9 / 0    strand density x0.8 / x1.25
    ///   , / .    tip thickness -/+ 0.02
    ///   O        root occlusion cycle (0 -> 0.2 -> 0.42 -> 0.7)
    bool on_fur_key(int key) {
        switch (key) {
            case GLFW_KEY_F:
                if (!fur_enabled_ && !fur_pass_.valid() && !create_fur_pass()) return true;
                fur_enabled_ = !fur_enabled_;
                std::printf("Fur shells: %s\n", fur_enabled_ ? "ON" : "OFF");
                return true;
            case GLFW_KEY_LEFT_BRACKET:
                if (fur_.shell_count > 1) --fur_.shell_count;
                break;
            case GLFW_KEY_RIGHT_BRACKET:
                if (fur_.shell_count < 32) ++fur_.shell_count;
                break;
            case GLFW_KEY_MINUS:  fur_.length *= 0.8f;  break;
            case GLFW_KEY_EQUAL:  fur_.length *= 1.25f; break;
            case GLFW_KEY_9:      fur_.density = std::max(8.0f, fur_.density * 0.8f);    break;
            case GLFW_KEY_0:      fur_.density = std::min(256.0f, fur_.density * 1.25f); break;
            case GLFW_KEY_COMMA:  fur_.tip_thickness = std::max(0.01f, fur_.tip_thickness - 0.02f); break;
            case GLFW_KEY_PERIOD: fur_.tip_thickness = std::min(0.5f,  fur_.tip_thickness + 0.02f); break;
            case GLFW_KEY_O:
                fur_.root_occlusion = fur_.root_occlusion < 0.1f ? 0.2f
                                    : fur_.root_occlusion < 0.3f ? 0.42f
                                    : fur_.root_occlusion < 0.5f ? 0.7f : 0.0f;
                break;
            default: return false;
        }
        fur_.print();
        return true;
    }

    // ─── depth buffer ───────────────────────────────────────
    bool create_depth_resources() {
        VkDevice d = vk_.device();
        VkExtent2D ext = vk_.swapchain_extent();
        const VkFormat depth_fmt = VK_FORMAT_D32_SFLOAT;

        VkImageCreateInfo ii{};
        ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ii.imageType = VK_IMAGE_TYPE_2D;
        ii.format    = depth_fmt;
        ii.extent    = {ext.width, ext.height, 1};
        ii.mipLevels = 1; ii.arrayLayers = 1;
        ii.samples   = VK_SAMPLE_COUNT_1_BIT;
        ii.tiling    = VK_IMAGE_TILING_OPTIMAL;
        ii.usage     = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vkCreateImage(d, &ii, nullptr, &depth_image_) != VK_SUCCESS) return false;

        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(d, depth_image_, &req);
        VkMemoryAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        ai.allocationSize  = req.size;
        ai.memoryTypeIndex = find_memory_type(vk_.physical_device(), req.memoryTypeBits,
                                              VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (vkAllocateMemory(d, &ai, nullptr, &depth_mem_) != VK_SUCCESS) return false;
        vkBindImageMemory(d, depth_image_, depth_mem_, 0);

        VkImageViewCreateInfo vi{};
        vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vi.image = depth_image_;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format   = depth_fmt;
        vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        vi.subresourceRange.levelCount = 1;
        vi.subresourceRange.layerCount = 1;
        return vkCreateImageView(d, &vi, nullptr, &depth_view_) == VK_SUCCESS;
    }

    void destroy_depth_resources() {
        VkDevice d = vk_.device();
        if (depth_view_)  { vkDestroyImageView(d, depth_view_,  nullptr); depth_view_  = VK_NULL_HANDLE; }
        if (depth_image_) { vkDestroyImage(d, depth_image_,     nullptr); depth_image_ = VK_NULL_HANDLE; }
        if (depth_mem_)   { vkFreeMemory(d, depth_mem_,         nullptr); depth_mem_   = VK_NULL_HANDLE; }
    }

    bool create_render_pass() {
        VkDevice d = vk_.device();
        VkAttachmentDescription attachments[2]{};
        attachments[0].format  = vk_.swapchain_format();
        attachments[0].samples = VK_SAMPLE_COUNT_1_BIT;
        attachments[0].loadOp  = VK_ATTACHMENT_LOAD_OP_CLEAR;
        attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        attachments[0].stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachments[0].initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
        attachments[0].finalLayout    = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        attachments[1].format  = VK_FORMAT_D32_SFLOAT;
        attachments[1].samples = VK_SAMPLE_COUNT_1_BIT;
        attachments[1].loadOp  = VK_ATTACHMENT_LOAD_OP_CLEAR;
        attachments[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachments[1].stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachments[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachments[1].initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
        attachments[1].finalLayout    = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

        VkAttachmentReference color_ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkAttachmentReference depth_ref{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
        VkSubpassDescription sub{};
        sub.pipelineBindPoint    = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub.colorAttachmentCount = 1;
        sub.pColorAttachments    = &color_ref;
        sub.pDepthStencilAttachment = &depth_ref;
        VkSubpassDependency dep{};
        dep.srcSubpass    = VK_SUBPASS_EXTERNAL;
        dep.dstSubpass    = 0;
        dep.srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
        dep.dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
        dep.srcAccessMask = 0;
        dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

        VkRenderPassCreateInfo rp{};
        rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        rp.attachmentCount = 2;
        rp.pAttachments = attachments;
        rp.subpassCount = 1;
        rp.pSubpasses = &sub;
        rp.dependencyCount = 1;
        rp.pDependencies = &dep;
        return vkCreateRenderPass(d, &rp, nullptr, &render_pass_) == VK_SUCCESS;
    }

    bool create_framebuffers() {
        VkDevice d = vk_.device();
        VkExtent2D ext = vk_.swapchain_extent();
        const auto& views = vk_.swapchain_image_views();
        framebuffers_.resize(views.size());
        for (size_t i = 0; i < views.size(); ++i) {
            VkImageView att[2] = {views[i], depth_view_};
            VkFramebufferCreateInfo fb{};
            fb.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
            fb.renderPass = render_pass_;
            fb.attachmentCount = 2;
            fb.pAttachments = att;
            fb.width  = ext.width;
            fb.height = ext.height;
            fb.layers = 1;
            if (vkCreateFramebuffer(d, &fb, nullptr, &framebuffers_[i]) != VK_SUCCESS) return false;
        }
        return true;
    }

    bool create_descriptor_layouts() {
        VkDevice d = vk_.device();
        // Set 0: UBO + instance SSBO + bone SSBO + rope-binding UBO
        VkDescriptorSetLayoutBinding s0[4]{};
        s0[0].binding = 0; s0[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; s0[0].descriptorCount = 1;
        s0[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        s0[1].binding = 1; s0[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; s0[1].descriptorCount = 1;
        s0[1].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        s0[2].binding = 2; s0[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; s0[2].descriptorCount = 1;
        s0[2].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        s0[3].binding = 3; s0[3].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; s0[3].descriptorCount = 1;
        s0[3].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        info.bindingCount = 4; info.pBindings = s0;
        if (vkCreateDescriptorSetLayout(d, &info, nullptr, &scene_set_layout_) != VK_SUCCESS) return false;

        // Set 1: combined image sampler (diffuse)
        VkDescriptorSetLayoutBinding s1{};
        s1.binding = 0; s1.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; s1.descriptorCount = 1;
        s1.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo info2{};
        info2.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        info2.bindingCount = 1; info2.pBindings = &s1;
        return vkCreateDescriptorSetLayout(d, &info2, nullptr, &tex_set_layout_) == VK_SUCCESS;
    }

    bool create_pipeline() {
        VkDevice d = vk_.device();

        VkShaderModule vs = load_shader_spv(d, shader_dir_ + "/model.vert.spv");
        VkShaderModule fs = load_shader_spv(d, shader_dir_ + "/model.frag.spv");
        if (!vs || !fs) return false;

        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT; stages[0].module = vs; stages[0].pName = "main";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = fs; stages[1].pName = "main";

        VkVertexInputBindingDescription binding{};
        binding.binding = 0;
        binding.stride  = TSV_STRIDE;
        binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

        VkVertexInputAttributeDescription attrs[5]{};
        attrs[0].location = 0; attrs[0].format = VK_FORMAT_R32G32B32_SFLOAT;    attrs[0].offset = TSV_OFFSET_POSITION;
        attrs[1].location = 1; attrs[1].format = VK_FORMAT_R32G32B32_SFLOAT;    attrs[1].offset = TSV_OFFSET_NORMAL;
        attrs[2].location = 2; attrs[2].format = VK_FORMAT_R32G32_SFLOAT;       attrs[2].offset = TSV_OFFSET_UV;
        attrs[3].location = 3; attrs[3].format = VK_FORMAT_R32G32B32A32_UINT;   attrs[3].offset = TSV_OFFSET_JOINTS;
        attrs[4].location = 4; attrs[4].format = VK_FORMAT_R32G32B32A32_SFLOAT; attrs[4].offset = TSV_OFFSET_WEIGHTS;

        VkPipelineVertexInputStateCreateInfo vi{};
        vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        vi.vertexBindingDescriptionCount = 1; vi.pVertexBindingDescriptions = &binding;
        vi.vertexAttributeDescriptionCount = 5; vi.pVertexAttributeDescriptions = attrs;

        VkPipelineInputAssemblyStateCreateInfo ia{};
        ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

        VkPipelineViewportStateCreateInfo vp{};
        vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        vp.viewportCount = 1; vp.scissorCount = 1;

        VkPipelineRasterizationStateCreateInfo rs{};
        rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rs.polygonMode = VK_POLYGON_MODE_FILL;
        rs.cullMode    = VK_CULL_MODE_NONE;   // clothes / hair often two-sided
        rs.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        rs.lineWidth   = 1.0f;

        VkPipelineMultisampleStateCreateInfo ms{};
        ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        VkPipelineDepthStencilStateCreateInfo ds{};
        ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        ds.depthTestEnable = VK_TRUE;
        ds.depthWriteEnable = VK_TRUE;
        ds.depthCompareOp  = VK_COMPARE_OP_LESS;

        VkPipelineColorBlendAttachmentState cba{};
        cba.colorWriteMask = 0xF;
        VkPipelineColorBlendStateCreateInfo cb{};
        cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        cb.attachmentCount = 1; cb.pAttachments = &cba;

        VkDynamicState dyn_states[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dyn{};
        dyn.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dyn.dynamicStateCount = 2; dyn.pDynamicStates = dyn_states;

        VkDescriptorSetLayout set_layouts[2] = {scene_set_layout_, tex_set_layout_};
        VkPipelineLayoutCreateInfo pl{};
        pl.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pl.setLayoutCount = 2; pl.pSetLayouts = set_layouts;
        if (vkCreatePipelineLayout(d, &pl, nullptr, &pipeline_layout_) != VK_SUCCESS) return false;

        VkGraphicsPipelineCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        info.stageCount = 2; info.pStages = stages;
        info.pVertexInputState = &vi;
        info.pInputAssemblyState = &ia;
        info.pViewportState = &vp;
        info.pRasterizationState = &rs;
        info.pMultisampleState = &ms;
        info.pDepthStencilState = &ds;
        info.pColorBlendState = &cb;
        info.pDynamicState = &dyn;
        info.layout = pipeline_layout_;
        info.renderPass = render_pass_;

        VkResult r = vkCreateGraphicsPipelines(d, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline_);
        vkDestroyShaderModule(d, vs, nullptr);
        vkDestroyShaderModule(d, fs, nullptr);
        return r == VK_SUCCESS;
    }

    // ─── bone debug overlay pipeline ─────────────────────────
    // Vertex layout: { vec3 position; vec3 color; } (24 bytes).
    // Shares the scene descriptor set (set 0) with the mesh pipeline.
    bool create_bone_pipeline() {
        VkDevice d = vk_.device();

        VkShaderModule vs = load_shader_spv(d, shader_dir_ + "/bone.vert.spv");
        VkShaderModule fs = load_shader_spv(d, shader_dir_ + "/bone.frag.spv");
        if (!vs || !fs) return false;

        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;   stages[0].module = vs; stages[0].pName = "main";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = fs; stages[1].pName = "main";

        VkVertexInputBindingDescription binding{};
        binding.binding = 0;
        binding.stride  = 24;
        binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
        VkVertexInputAttributeDescription attrs[2]{};
        attrs[0].location = 0; attrs[0].format = VK_FORMAT_R32G32B32_SFLOAT; attrs[0].offset = 0;
        attrs[1].location = 1; attrs[1].format = VK_FORMAT_R32G32B32_SFLOAT; attrs[1].offset = 12;

        VkPipelineVertexInputStateCreateInfo vi{};
        vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        vi.vertexBindingDescriptionCount   = 1; vi.pVertexBindingDescriptions   = &binding;
        vi.vertexAttributeDescriptionCount = 2; vi.pVertexAttributeDescriptions = attrs;

        VkPipelineInputAssemblyStateCreateInfo ia{};
        ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        ia.topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;

        VkPipelineViewportStateCreateInfo vp{};
        vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        vp.viewportCount = 1; vp.scissorCount = 1;

        VkPipelineRasterizationStateCreateInfo rs{};
        rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rs.polygonMode = VK_POLYGON_MODE_FILL;
        rs.cullMode    = VK_CULL_MODE_NONE;
        rs.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        rs.lineWidth   = 1.0f;

        VkPipelineMultisampleStateCreateInfo ms{};
        ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        // Depth test OFF so bones are always visible through the mesh.
        VkPipelineDepthStencilStateCreateInfo ds{};
        ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        ds.depthTestEnable  = VK_FALSE;
        ds.depthWriteEnable = VK_FALSE;
        ds.depthCompareOp   = VK_COMPARE_OP_ALWAYS;

        VkPipelineColorBlendAttachmentState cba{};
        cba.colorWriteMask = 0xF;
        VkPipelineColorBlendStateCreateInfo cb{};
        cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        cb.attachmentCount = 1; cb.pAttachments = &cba;

        VkDynamicState dyn_states[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dyn{};
        dyn.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dyn.dynamicStateCount = 2; dyn.pDynamicStates = dyn_states;

        VkPipelineLayoutCreateInfo pl{};
        pl.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pl.setLayoutCount = 1; pl.pSetLayouts = &scene_set_layout_;
        if (vkCreatePipelineLayout(d, &pl, nullptr, &debug_pipeline_layout_) != VK_SUCCESS) return false;

        VkGraphicsPipelineCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        info.stageCount = 2; info.pStages = stages;
        info.pVertexInputState = &vi;
        info.pInputAssemblyState = &ia;
        info.pViewportState = &vp;
        info.pRasterizationState = &rs;
        info.pMultisampleState = &ms;
        info.pDepthStencilState = &ds;
        info.pColorBlendState = &cb;
        info.pDynamicState = &dyn;
        info.layout = debug_pipeline_layout_;
        info.renderPass = render_pass_;

        VkResult r = vkCreateGraphicsPipelines(d, VK_NULL_HANDLE, 1, &info, nullptr, &debug_pipeline_);
        vkDestroyShaderModule(d, vs, nullptr);
        vkDestroyShaderModule(d, fs, nullptr);
        return r == VK_SUCCESS;
    }

    bool create_fur_pass() {
        if (fur_pass_.valid()) return true;
        FurShellPass::CreateInfo ci;
        ci.device       = vk_.device();
        ci.render_pass  = render_pass_;
        ci.scene_layout = scene_set_layout_;
        ci.tex_layout   = tex_set_layout_;
        ci.shader_dir   = shader_dir_;
        if (!fur_pass_.create(ci)) {
            std::fprintf(stderr, "Fur shell pipeline unavailable (missing fur_shell.*.spv?)\n");
            return false;
        }
        return true;
    }

    bool create_rope_pass() {
        if (rope_pass_.valid()) return true;
        RopePass::CreateInfo ci;
        ci.device          = vk_.device();
        ci.physical_device = vk_.physical_device();
        ci.render_pass     = render_pass_;
        ci.scene_layout    = scene_set_layout_;
        ci.shader_dir      = shader_dir_;
        if (!rope_pass_.create(ci)) {
            std::fprintf(stderr, "Rope pipeline unavailable (missing rope.*.spv?)\n");
            return false;
        }
        return true;
    }

    bool create_buffers() {
        VkDevice d = vk_.device();
        VkPhysicalDevice pd = vk_.physical_device();
        const VkMemoryPropertyFlags hv = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

        if (mesh_.vertices.empty() || mesh_.indices.empty()) {
            std::fprintf(stderr, "No mesh data to upload.\n");
            return false;
        }

        VkDeviceSize vb_size = mesh_.vertices.size() * sizeof(TexturedSkinnedVertex);
        if (!create_buffer(d, pd, vb_size, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, hv, vb_, vb_mem_)) return false;
        void* p = nullptr;
        vkMapMemory(d, vb_mem_, 0, vb_size, 0, &p);
        std::memcpy(p, mesh_.vertices.data(), vb_size);
        vkUnmapMemory(d, vb_mem_);

        VkDeviceSize ib_size = mesh_.indices.size() * sizeof(uint32_t);
        if (!create_buffer(d, pd, ib_size, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, hv, ib_, ib_mem_)) return false;
        vkMapMemory(d, ib_mem_, 0, ib_size, 0, &p);
        std::memcpy(p, mesh_.indices.data(), ib_size);
        vkUnmapMemory(d, ib_mem_);

        if (!create_buffer(d, pd, sizeof(SceneUBO), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, hv,
                           ubo_buffer_, ubo_mem_)) return false;
        if (!create_buffer(d, pd, sizeof(InstanceData), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, hv,
                           instance_buffer_, instance_mem_)) return false;
        if (!create_buffer(d, pd, sizeof(float) * 16 * kMaxBones, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, hv,
                           bone_buffer_, bone_mem_)) return false;
        if (!create_buffer(d, pd, sizeof(NawaBindUBO), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, hv,
                           bind_buffer_, bind_mem_)) return false;
        return true;
    }

    // ─── load textures referenced by submesh materials ───────
    bool load_textures() {
        VkDevice d = vk_.device();
        VkPhysicalDevice pd = vk_.physical_device();
        VkCommandPool pool = vk_.command_pool();
        VkQueue q = vk_.graphics_queue();

        // Fallback 1x1 white texture for missing diffuse maps.
        uint8_t white[4] = {255, 255, 255, 255};
        if (!upload_texture_rgba8(d, pd, pool, q, white, 1, 1, fallback_texture_)) {
            std::fprintf(stderr, "Failed to create fallback texture.\n");
            return false;
        }

        const fs::path tex_dir = model_dir_ / "texture";
        int loaded = 0, missing = 0;

        for (const SubMesh& sm : mesh_.submeshes) {
            // Materials whose FBX texture path is unusable (e.g. an absolute
            // path from the artist's machine) fall back to texture/default.*
            // when the model directory provides one.
            std::string base = sm.texture_basename;
            std::string resolved = base.empty() ? std::string() : find_texture_file(tex_dir, base);
            if (resolved.empty()) {
                std::string fallback = find_texture_file(tex_dir, "default");
                if (!fallback.empty()) {
                    std::printf("  [default] %s -> default texture\n",
                                base.empty() ? "(no material texture)" : base.c_str());
                    base = "default";
                    resolved = fallback;
                }
            }
            if (base.empty()) { submesh_tex_keys_.push_back(""); continue; }

            if (resolved.empty()) {
                std::printf("  [miss] %s\n", base.c_str());
                ++missing;
                submesh_tex_keys_.push_back("");
                continue;
            }

            int w, h, nch;
            stbi_uc* pixels = stbi_load(resolved.c_str(), &w, &h, &nch, STBI_rgb_alpha);
            // Score every textured submesh, including those sharing a cached
            // fallback texture. The face is the submesh whose UVs most closely
            // cover both detected eye blobs; iteration order is not stable.
            if (pixels) {
                std::vector<EyeAnchor> candidate = locate_eyes(
                    pixels, w, h, mesh_.vertices, mesh_.indices,
                    sm.index_start, sm.index_count);
                auto score = [](const std::vector<EyeAnchor>& anchors) {
                    float total = 0.0f;
                    for (const EyeAnchor& anchor : anchors) total += anchor.uv_distance_sq;
                    return total;
                };
                const float candidate_score = score(candidate);
                const float selected_score = score(eye_anchors_);
                const bool deterministic_tie = !candidate.empty() && !eye_anchors_.empty() &&
                    std::fabs(candidate_score - selected_score) <= 1e-8f &&
                    candidate.front().vertex_index < eye_anchors_.front().vertex_index;
                if (candidate.size() > eye_anchors_.size() ||
                    (candidate.size() == eye_anchors_.size() && !candidate.empty() &&
                     (candidate_score < selected_score - 1e-8f || deterministic_tie))) {
                    eye_anchors_ = std::move(candidate);
                }
            }
            if (textures_.count(base)) {
                if (pixels) stbi_image_free(pixels);
                submesh_tex_keys_.push_back(base);
                continue;
            }
            if (!pixels) {
                std::printf("  [fail] %s (%s)\n", base.c_str(), stbi_failure_reason());
                ++missing;
                submesh_tex_keys_.push_back("");
                continue;
            }
            GPUTexture gt{};
            bool ok = upload_texture_rgba8(d, pd, pool, q, pixels,
                                           static_cast<uint32_t>(w),
                                           static_cast<uint32_t>(h), gt);
            stbi_image_free(pixels);
            if (!ok) {
                std::fprintf(stderr, "  [gpu-fail] %s\n", base.c_str());
                submesh_tex_keys_.push_back("");
                continue;
            }
            textures_[base] = gt;
            submesh_tex_keys_.push_back(base);
            std::printf("  [load] %s (%dx%d)\n", base.c_str(), w, h);
            ++loaded;
        }
        std::printf("Textures: %d loaded, %d missing/fallback.\n", loaded, missing);
        return true;
    }

    bool create_descriptor_sets() {
        VkDevice d = vk_.device();
        const uint32_t sub_count = static_cast<uint32_t>(mesh_.submeshes.size());

        VkDescriptorPoolSize sz[3]{};
        sz[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; sz[0].descriptorCount = 2;
        sz[1].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; sz[1].descriptorCount = 2;
        sz[2].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; sz[2].descriptorCount = sub_count;
        VkDescriptorPoolCreateInfo dp{};
        dp.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        dp.poolSizeCount = 3; dp.pPoolSizes = sz; dp.maxSets = 1 + sub_count;
        if (vkCreateDescriptorPool(d, &dp, nullptr, &desc_pool_) != VK_SUCCESS) return false;

        // Scene set (set 0)
        VkDescriptorSetAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        ai.descriptorPool = desc_pool_;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &scene_set_layout_;
        if (vkAllocateDescriptorSets(d, &ai, &scene_set_) != VK_SUCCESS) return false;

        VkDescriptorBufferInfo ubo_info{ubo_buffer_, 0, sizeof(SceneUBO)};
        VkDescriptorBufferInfo inst_info{instance_buffer_, 0, VK_WHOLE_SIZE};
        VkDescriptorBufferInfo bone_info{bone_buffer_, 0, VK_WHOLE_SIZE};
        VkDescriptorBufferInfo bind_info{bind_buffer_, 0, sizeof(NawaBindUBO)};
        VkWriteDescriptorSet w[4]{};
        w[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[0].dstSet = scene_set_; w[0].dstBinding = 0;
        w[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; w[0].descriptorCount = 1; w[0].pBufferInfo = &ubo_info;
        w[1] = w[0]; w[1].dstBinding = 1; w[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[1].pBufferInfo = &inst_info;
        w[2] = w[0]; w[2].dstBinding = 2; w[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[2].pBufferInfo = &bone_info;
        w[3] = w[0]; w[3].dstBinding = 3; w[3].pBufferInfo = &bind_info;
        vkUpdateDescriptorSets(d, 4, w, 0, nullptr);

        // Texture sets (set 1, one per submesh)
        submesh_tex_sets_.resize(sub_count, VK_NULL_HANDLE);
        for (uint32_t i = 0; i < sub_count; ++i) {
            VkDescriptorSetAllocateInfo tai{};
            tai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            tai.descriptorPool = desc_pool_;
            tai.descriptorSetCount = 1;
            tai.pSetLayouts = &tex_set_layout_;
            if (vkAllocateDescriptorSets(d, &tai, &submesh_tex_sets_[i]) != VK_SUCCESS) return false;

            const GPUTexture* tex = &fallback_texture_;
            const std::string& key = submesh_tex_keys_[i];
            if (!key.empty()) {
                auto it = textures_.find(key);
                if (it != textures_.end()) tex = &it->second;
            }
            VkDescriptorImageInfo di{};
            di.sampler = tex->sampler;
            di.imageView = tex->view;
            di.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            VkWriteDescriptorSet tw{};
            tw.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            tw.dstSet = submesh_tex_sets_[i]; tw.dstBinding = 0;
            tw.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            tw.descriptorCount = 1; tw.pImageInfo = &di;
            vkUpdateDescriptorSets(d, 1, &tw, 0, nullptr);
        }
        return true;
    }

    void update_uniforms(float t) {
        VkDevice d = vk_.device();
        VkExtent2D ext = vk_.swapchain_extent();
        float aspect = (ext.height == 0) ? 1.0f : static_cast<float>(ext.width) / ext.height;
        float dist = mesh_.radius * 2.5f + 0.1f;
        float eye[3] = {
            mesh_.center.x + dist * std::cos(t * 0.4f),
            mesh_.center.y + mesh_.radius * 0.5f,
            mesh_.center.z + dist * std::sin(t * 0.4f),
        };
        float center[3] = {mesh_.center.x, mesh_.center.y, mesh_.center.z};
        float up[3] = {0, 1, 0};

        SceneUBO ubo{};
        mat4_look_at(ubo.view, eye, center, up);
        mat4_perspective(ubo.proj, 45.0f * 3.14159265f / 180.0f, aspect, 0.1f, dist * 8.0f + 1000.0f);
        ubo.light_dir[0] = 0.4f; ubo.light_dir[1] = 0.8f; ubo.light_dir[2] = 0.5f; ubo.light_dir[3] = 1.0f;
        float l = std::sqrt(ubo.light_dir[0]*ubo.light_dir[0] + ubo.light_dir[1]*ubo.light_dir[1] + ubo.light_dir[2]*ubo.light_dir[2]);
        ubo.light_dir[0] /= l; ubo.light_dir[1] /= l; ubo.light_dir[2] /= l;
        ubo.light_color[0] = 1.0f; ubo.light_color[1] = 0.97f; ubo.light_color[2] = 0.93f; ubo.light_color[3] = 0.3f;
        ubo.camera_pos[0] = eye[0]; ubo.camera_pos[1] = eye[1]; ubo.camera_pos[2] = eye[2]; ubo.camera_pos[3] = 1.0f;
        camera_pos_ = {eye[0], eye[1], eye[2]};
        void* p = nullptr;
        vkMapMemory(d, ubo_mem_, 0, sizeof(SceneUBO), 0, &p);
        std::memcpy(p, &ubo, sizeof(SceneUBO));
        vkUnmapMemory(d, ubo_mem_);

        InstanceData inst{};
        mat4_identity(inst.model);
        inst.base_color[0] = 1.0f; inst.base_color[1] = 1.0f; inst.base_color[2] = 1.0f; inst.base_color[3] = 1.0f;
        inst.skin_info[0] = 0;
        inst.skin_info[1] = skinning_enabled_
                              ? static_cast<uint32_t>(anim_.get_bone_count(inst_))
                              : static_cast<uint32_t>(skeleton_.bones.size());
        vkMapMemory(d, instance_mem_, 0, sizeof(InstanceData), 0, &p);
        std::memcpy(p, &inst, sizeof(InstanceData));
        vkUnmapMemory(d, instance_mem_);

        // Bone matrices: either driven by AnimationSystem (skinning on) or
        // all-identity (skinning off → bind-pose pass-through in the shader).
        std::vector<float> bones(16 * kMaxBones, 0.0f);
        for (uint32_t b = 0; b < kMaxBones; ++b)
            bones[b * 16 + 0] = bones[b * 16 + 5] = bones[b * 16 + 10] = bones[b * 16 + 15] = 1.0f;
        if (skinning_enabled_) {
            const uint32_t bone_count = std::min<uint32_t>(anim_.get_bone_count(inst_), kMaxBones);
            const float4x4* sk = anim_.get_skinning_matrices(inst_);
            if (sk) {
                for (uint32_t b = 0; b < bone_count; ++b) pictor_mat_to_glsl(&bones[b * 16], sk[b]);
            }
        }
        vkMapMemory(d, bone_mem_, 0, sizeof(float) * 16 * kMaxBones, 0, &p);
        std::memcpy(p, bones.data(), sizeof(float) * 16 * kMaxBones);
        vkUnmapMemory(d, bone_mem_);
    }

    // ─── bone overlay vertex buffer update ────────────────────
    //
    // Emits three layers of lines (only when show_bones_ is ON):
    //   BIND pose skeleton  (grey, from Bone::bind_pose hierarchy)
    //   CURRENT pose skeleton (green when skinning ON, else same as bind)
    //   World-origin axes   (red/green/blue, always at 0)
    //
    // Producing both the bind and current skeletons lets us visually tell
    // whether the animation moved the bones correctly independent of the
    // skin mesh.
    void update_debug_bones() {
        debug_vertex_count_ = 0;
        if (!show_bones_ || skeleton_.bones.empty()) return;

        struct V { float p[3]; float c[3]; };
        std::vector<V> verts;
        const size_t n = skeleton_.bones.size();
        verts.reserve(n * 2 * 2 + 6);

        // World-origin axes (1 unit long).
        verts.push_back({{0,0,0},{1,0,0}}); verts.push_back({{1,0,0},{1,0,0}});
        verts.push_back({{0,0,0},{0,1,0}}); verts.push_back({{0,1,0},{0,1,0}});
        verts.push_back({{0,0,0},{0,0,1}}); verts.push_back({{0,0,1},{0,0,1}});

        // BIND pose: compute by cascading bind_pose TRS down the hierarchy.
        // Row-vector convention: world_i = local_i * parent_world.
        std::vector<float4x4> bind_world(n);
        for (size_t i = 0; i < n; ++i) {
            const Bone& b = skeleton_.bones[i];
            float4x4 local = b.bind_pose.to_matrix();
            if (b.parent_index < 0) {
                bind_world[i] = local;
            } else {
                const float4x4& parent = bind_world[b.parent_index];
                float4x4& dst = bind_world[i];
                for (int r = 0; r < 4; ++r)
                    for (int c = 0; c < 4; ++c) {
                        dst.m[r][c] = 0;
                        for (int k = 0; k < 4; ++k)
                            dst.m[r][c] += local.m[r][k] * parent.m[k][c];
                    }
            }
        }
        auto world_pos = [](const float4x4& m) {
            return float3{m.m[3][0], m.m[3][1], m.m[3][2]};
        };

        const float3 bind_color{0.35f, 0.35f, 0.35f};
        for (size_t i = 0; i < n; ++i) {
            const Bone& b = skeleton_.bones[i];
            if (b.parent_index < 0) continue;
            float3 cp = world_pos(bind_world[i]);
            float3 pp = world_pos(bind_world[b.parent_index]);
            verts.push_back({{pp.x, pp.y, pp.z},{bind_color.x, bind_color.y, bind_color.z}});
            verts.push_back({{cp.x, cp.y, cp.z},{bind_color.x, bind_color.y, bind_color.z}});
        }

        // CURRENT pose: pull from AnimationSystem when skinning is on.
        // When skinning is off the current pose equals bind, so skip to
        // avoid overlapping lines.
        if (skinning_enabled_) {
            const float4x4* cur = anim_.get_world_matrices(inst_);
            if (cur) {
                const size_t bc = std::min<size_t>(anim_.get_bone_count(inst_), n);
                const float3 cur_color{0.25f, 1.0f, 0.35f};
                for (size_t i = 0; i < bc; ++i) {
                    const Bone& b = skeleton_.bones[i];
                    if (b.parent_index < 0) continue;
                    if (static_cast<size_t>(b.parent_index) >= bc) continue;
                    float3 cp = world_pos(cur[i]);
                    float3 pp = world_pos(cur[b.parent_index]);
                    verts.push_back({{pp.x, pp.y, pp.z},{cur_color.x, cur_color.y, cur_color.z}});
                    verts.push_back({{cp.x, cp.y, cp.z},{cur_color.x, cur_color.y, cur_color.z}});
                }
            }
        }

        debug_vertex_count_ = static_cast<uint32_t>(verts.size());
        if (debug_vertex_count_ == 0) return;

        // (Re)allocate the vertex buffer if it needs to grow.
        VkDevice d = vk_.device();
        if (debug_vertex_count_ > debug_vb_capacity_) {
            if (debug_vb_)     { vkDestroyBuffer(d, debug_vb_, nullptr);     debug_vb_ = VK_NULL_HANDLE; }
            if (debug_vb_mem_) { vkFreeMemory(d, debug_vb_mem_, nullptr);    debug_vb_mem_ = VK_NULL_HANDLE; }
            debug_vb_capacity_ = debug_vertex_count_ * 2;
            const VkDeviceSize sz = static_cast<VkDeviceSize>(debug_vb_capacity_) * 24;
            const VkMemoryPropertyFlags hv = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
            if (!create_buffer(d, vk_.physical_device(), sz, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, hv,
                               debug_vb_, debug_vb_mem_)) {
                debug_vertex_count_ = 0;
                return;
            }
        }
        void* p = nullptr;
        vkMapMemory(d, debug_vb_mem_, 0, debug_vertex_count_ * 24, 0, &p);
        std::memcpy(p, verts.data(), debug_vertex_count_ * 24);
        vkUnmapMemory(d, debug_vb_mem_);
    }

    void record_and_submit(uint32_t img_idx) {
        VkCommandBuffer cmd = vk_.command_buffers()[img_idx];
        VkExtent2D ext = vk_.swapchain_extent();

        vkResetCommandBuffer(cmd, 0);
        VkCommandBufferBeginInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        vkBeginCommandBuffer(cmd, &bi);

        VkClearValue clears[2];
        clears[0].color = {{0.08f, 0.09f, 0.12f, 1.0f}};
        clears[1].depthStencil = {1.0f, 0};

        VkRenderPassBeginInfo rp{};
        rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rp.renderPass = render_pass_;
        rp.framebuffer = framebuffers_[img_idx];
        rp.renderArea = {{0, 0}, ext};
        rp.clearValueCount = 2;
        rp.pClearValues = clears;
        vkCmdBeginRenderPass(cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);

        VkViewport vp{0, 0, static_cast<float>(ext.width), static_cast<float>(ext.height), 0, 1};
        VkRect2D sc{{0, 0}, ext};
        vkCmdSetViewport(cmd, 0, 1, &vp);
        vkCmdSetScissor(cmd, 0, 1, &sc);

        if (show_mesh_) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout_,
                                    0, 1, &scene_set_, 0, nullptr);
            VkDeviceSize off = 0;
            vkCmdBindVertexBuffers(cmd, 0, 1, &vb_, &off);
            vkCmdBindIndexBuffer(cmd, ib_, 0, VK_INDEX_TYPE_UINT32);

            for (size_t i = 0; i < mesh_.submeshes.size(); ++i) {
                const SubMesh& sm = mesh_.submeshes[i];
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout_,
                                        1, 1, &submesh_tex_sets_[i], 0, nullptr);
                vkCmdDrawIndexed(cmd, sm.index_count, 1, sm.index_start, 0, 0);
            }

            if (fur_enabled_ && fur_pass_.valid()) {
                std::vector<FurShellSubmeshDraw> draws;
                draws.reserve(mesh_.submeshes.size());
                for (size_t i = 0; i < mesh_.submeshes.size(); ++i) {
                    const SubMesh& sm = mesh_.submeshes[i];
                    draws.push_back({sm.index_start, sm.index_count, submesh_tex_sets_[i]});
                }
                fur_pass_.record(cmd, scene_set_, draws, fur_);
            }
        }

        if (bind_enabled_ && show_rope_) rope_pass_.record(cmd, scene_set_);
        if (tears_enabled_) tear_pass_.record(cmd, scene_set_);

        if (show_bones_ && debug_vertex_count_ > 0 && debug_vb_ != VK_NULL_HANDLE) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, debug_pipeline_);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, debug_pipeline_layout_,
                                    0, 1, &scene_set_, 0, nullptr);
            VkDeviceSize doff = 0;
            vkCmdBindVertexBuffers(cmd, 0, 1, &debug_vb_, &doff);
            vkCmdDraw(cmd, debug_vertex_count_, 1, 0, 0);
        }

        vkCmdEndRenderPass(cmd);

        if (capture_.should_capture(frame_index_)) {
            const auto& images = vk_.swapchain_images();
            if (img_idx < images.size() &&
                capture_.record(cmd, vk_.device(), vk_.physical_device(), images[img_idx], ext,
                                vk_.swapchain_format())) {
                capture_pending_ = true;
            }
        }

        vkEndCommandBuffer(cmd);

        VkSemaphore wait_sem = vk_.image_available_semaphore();
        VkSemaphore sig_sem  = vk_.render_finished_semaphore();
        VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo si{};
        si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.waitSemaphoreCount = 1; si.pWaitSemaphores = &wait_sem; si.pWaitDstStageMask = &wait_stage;
        si.commandBufferCount = 1; si.pCommandBuffers = &cmd;
        si.signalSemaphoreCount = 1; si.pSignalSemaphores = &sig_sem;
        vkQueueSubmit(vk_.graphics_queue(), 1, &si, vk_.in_flight_fence());
    }

    void handle_resize() {
        VkDevice d = vk_.device();
        vkDeviceWaitIdle(d);
        for (auto fb : framebuffers_) if (fb) vkDestroyFramebuffer(d, fb, nullptr);
        framebuffers_.clear();
        destroy_depth_resources();
        vk_.recreate_swapchain();
        create_depth_resources();
        create_framebuffers();
    }

private:
    GlfwSurfaceProvider provider_;
    VulkanContext       vk_;

    PackedMesh                            mesh_;
    SkeletonDescriptor                    skeleton_;
    std::vector<AnimationClipDescriptor>  clips_;
    fs::path         model_dir_;
    std::string      shader_dir_;

    AnimationSystem                   anim_;
    SkeletonHandle                    skel_ = INVALID_SKELETON;
    std::vector<AnimationClipHandle>  clip_handles_;
    AnimationStateHandle              inst_ = INVALID_ANIMATION_STATE;
    size_t                            clip_index_ = 0;
    bool                              skinning_enabled_ = true;

    VkRenderPass                render_pass_ = VK_NULL_HANDLE;
    std::vector<VkFramebuffer>  framebuffers_;
    VkImage         depth_image_ = VK_NULL_HANDLE;
    VkDeviceMemory  depth_mem_   = VK_NULL_HANDLE;
    VkImageView     depth_view_  = VK_NULL_HANDLE;

    VkDescriptorSetLayout scene_set_layout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout tex_set_layout_   = VK_NULL_HANDLE;
    VkDescriptorPool      desc_pool_        = VK_NULL_HANDLE;
    VkDescriptorSet       scene_set_        = VK_NULL_HANDLE;
    std::vector<VkDescriptorSet>            submesh_tex_sets_;
    std::vector<std::string>                submesh_tex_keys_;

    VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;
    VkPipeline       pipeline_        = VK_NULL_HANDLE;

    VkBuffer        ubo_buffer_ = VK_NULL_HANDLE; VkDeviceMemory ubo_mem_      = VK_NULL_HANDLE;
    VkBuffer        vb_         = VK_NULL_HANDLE; VkDeviceMemory vb_mem_       = VK_NULL_HANDLE;
    VkBuffer        ib_         = VK_NULL_HANDLE; VkDeviceMemory ib_mem_       = VK_NULL_HANDLE;
    VkBuffer        instance_buffer_ = VK_NULL_HANDLE; VkDeviceMemory instance_mem_ = VK_NULL_HANDLE;
    VkBuffer        bone_buffer_     = VK_NULL_HANDLE; VkDeviceMemory bone_mem_     = VK_NULL_HANDLE;

    std::unordered_map<std::string, GPUTexture> textures_;
    GPUTexture                                  fallback_texture_{};

    // ── Bone debug overlay ──
    VkPipeline       debug_pipeline_         = VK_NULL_HANDLE;
    VkPipelineLayout debug_pipeline_layout_  = VK_NULL_HANDLE;
    VkBuffer         debug_vb_               = VK_NULL_HANDLE;
    VkDeviceMemory   debug_vb_mem_           = VK_NULL_HANDLE;
    uint32_t         debug_vb_capacity_      = 0;  // in vertices
    uint32_t         debug_vertex_count_     = 0;
    bool             show_bones_             = true;
    bool             show_mesh_              = true;

    // -- Fur shells / capture --
    ViewerOptions    options_;
    FurShellPass     fur_pass_;
    FurShellParams   fur_;
    bool             fur_enabled_            = false;
    RopePass           rope_pass_;
    BindingFrame       bind_frame_;
    BindingBodyProfile bind_base_profile_;   // plush preset in model units
    BindingBodyProfile bind_profile_;        // per-pattern adjusted (Obi pinches wider)
    BindingPatternKind bind_pattern_ = BindingPatternKind::Obi;
    NawaCapsuleChain   bind_chain_;
    SurfaceRadiusFn    bind_surface_radius_;
    uint32_t           bind_visible_segments_ = 0;
    float              bind_tightness_ = 1.0f;
    bool               bind_enabled_   = false;
    bool               show_rope_      = true;
    bool               rope_tail_      = true;
    float3             camera_pos_{};
    TearPass           tear_pass_;
    TearPass::Params   tear_params_;
    std::vector<EyeAnchor> eye_anchors_;
    bool               tears_enabled_  = false;
    VkBuffer           bind_buffer_ = VK_NULL_HANDLE; VkDeviceMemory bind_mem_ = VK_NULL_HANDLE;
    FrameCapture     capture_;
    bool             capture_pending_        = false;
    uint32_t         frame_index_            = 0;
};

// ============================================================
// main
// ============================================================

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);
    std::printf("Pictor FBX Viewer\n");

    // Argument parsing: [path] [shader_dir]. `path` can be a directory
    // (we load model.fbx + animation/*.fbx from it) or a single .fbx file.
    // Positional: [path] [shader_dir]. Options (anywhere):
    //   --fur                 start with fur shells on
    //   --shells N            shell count (1..32)
    //   --fur-length X        outermost shell extrusion in model units
    //   --fur-density X       strands per UV unit
    //   --capture FILE.bmp    write a frame capture, then exit
    //   --capture-frame N     frame index to capture (default 90)
    //   --stay                keep running after the capture
    //   --no-bones            start with the bone overlay hidden
    //   --bind [pattern]      rope binding on (obi | guruguru | kikkou | tasuki, default obi = 3 waist loops)
    //   --tightness X         binding tightness 0..1 (default 1)
    //   --bind-anim           animate wrap + tighten from t=0
    //   --no-rope             hide the rope tube (deformation only)
    //   --no-rope-tail        no loose rope end trailing toward the camera
    //   --tears               toon tears from the eyes found in the albedo
    std::vector<std::string> positional;
    ViewerOptions options;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&](const char* what) -> const char* {
            if (i + 1 >= argc) { std::fprintf(stderr, "%s needs a value\n", what); std::exit(2); }
            return argv[++i];
        };
        if      (a == "--fur")           options.fur_enabled = true;
        else if (a == "--shells") {
            if (!parse_uint_option(next("--shells"), 1, 32, options.fur.shell_count)) {
                std::fprintf(stderr, "--shells must be an integer from 1 to 32\n"); return 2;
            }
            options.fur_enabled = true;
        }
        else if (a == "--fur-length") {
            if (!parse_float_option(next("--fur-length"), std::numeric_limits<float>::min(),
                                    std::numeric_limits<float>::max(), options.fur.length)) {
                std::fprintf(stderr, "--fur-length must be a positive finite number\n"); return 2;
            }
            options.fur_length_set = true; options.fur_enabled = true;
        }
        else if (a == "--fur-density") {
            if (!parse_float_option(next("--fur-density"), 1.0f, 1024.0f, options.fur.density)) {
                std::fprintf(stderr, "--fur-density must be from 1 to 1024\n"); return 2;
            }
            options.fur_enabled = true;
        }
        else if (a == "--capture")       options.capture_path = next("--capture");
        else if (a == "--capture-frame") {
            if (!parse_uint_option(next("--capture-frame"), 0, UINT32_MAX, options.capture_frame)) {
                std::fprintf(stderr, "--capture-frame must be a non-negative integer\n"); return 2;
            }
        }
        else if (a == "--stay")          options.exit_after_capture = false;
        else if (a == "--no-bones")      options.show_bones = false;
        else if (a == "--bind") {
            options.bind_enabled = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                if (!parse_binding_pattern(argv[++i], options.bind_pattern)) {
                    std::fprintf(stderr, "--bind pattern must be obi|guruguru|kikkou|tasuki\n"); return 2;
                }
            }
        }
        else if (a == "--tightness") {
            if (!parse_float_option(next("--tightness"), 0.0f, 1.0f, options.bind_tightness)) {
                std::fprintf(stderr, "--tightness must be from 0 to 1\n"); return 2;
            }
            options.bind_enabled = true;
        }
        else if (a == "--bind-anim")     { options.bind_wrap_anim = true; options.bind_enabled = true; }
        else if (a == "--no-rope")       options.show_rope = false;
        else if (a == "--no-rope-tail")  options.rope_tail = false;
        else if (a == "--tears")         options.tears = true;
        else if (!a.empty() && a[0] == '-') { std::fprintf(stderr, "Unknown option: %s\n", a.c_str()); return 2; }
        else positional.push_back(a);
    }
    fs::path input_path = (positional.size() >= 1) ? fs::path(positional[0]) : fs::path("fbx/model1");
    std::string shader_dir = (positional.size() >= 2) ? positional[1] : "shaders";

    fs::path model_dir;
    fs::path model_file;
    if (fs::is_directory(input_path)) {
        model_dir  = input_path;
        model_file = model_dir / "model.fbx";
    } else {
        model_file = input_path;
        model_dir  = input_path.parent_path();
    }

    if (!fs::exists(model_file)) {
        std::fprintf(stderr, "Model file not found: %s\n", model_file.string().c_str());
        return 1;
    }
    std::printf("Model directory: %s\n", model_dir.string().c_str());
    std::printf("Loading: %s\n", model_file.string().c_str());

    // ── Collect source files (model + animations) + their mtimes for
    //    cache freshness. Animation list is sorted lexicographically so
    //    the cache path is deterministic.
    std::vector<cache::SourceEntry> sources;
    sources.push_back({model_file.string(), cache::file_mtime_ns(model_file)});
    fs::path anim_dir = model_dir / "animation";
    std::vector<fs::path> anim_files;
    if (fs::is_directory(anim_dir)) {
        std::error_code ec;
        for (const auto& e : fs::directory_iterator(anim_dir, ec)) {
            if (!e.is_regular_file()) continue;
            auto ext = e.path().extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(),
                           [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
            if (ext == ".fbx") anim_files.push_back(e.path());
        }
        std::sort(anim_files.begin(), anim_files.end());
        for (const auto& af : anim_files) {
            sources.push_back({af.string(), cache::file_mtime_ns(af)});
        }
    }
    const fs::path cache_path = model_dir / ".cache" / "viewer_cache.bin";

    PackedMesh                              mesh;
    SkeletonDescriptor                      skeleton;
    std::vector<AnimationClipDescriptor>    clips;
    bool cache_hit = false;
    {
        PROF_SCOPE("cache read (try)");
        cache_hit = cache::read(cache_path, sources, mesh, skeleton, clips);
    }

    if (cache_hit) {
        std::printf("Cache hit: %s\n", cache_path.string().c_str());
    } else {
        std::printf("Cache miss — parsing FBX (%zu source file%s).\n",
                    sources.size(), sources.size() == 1 ? "" : "s");

        FBXImporter importer;
        FBXImportResult result;
        {
            PROF_SCOPE("fbx import (main)");
            result = importer.import_file(model_file.string());
        }
        if (!result.success) {
            std::fprintf(stderr, "FBX import failed: %s\n", result.error_message.c_str());
            return 1;
        }
        if (result.scene) {
            const FBXGlobalSettings& gs = result.scene->global_settings;
            static const char* axis_name[3] = {"X", "Y", "Z"};
            std::printf("FBX global: UpAxis=%s%s  FrontAxis=%s%s  CoordAxis=%s%s  UnitScale=%.4f (cm/unit)  FrameRate=%.2f\n",
                        (gs.up_axis_sign    < 0 ? "-" : "+"), axis_name[gs.up_axis    % 3],
                        (gs.front_axis_sign < 0 ? "-" : "+"), axis_name[gs.front_axis % 3],
                        (gs.coord_axis_sign < 0 ? "-" : "+"), axis_name[gs.coord_axis % 3],
                        gs.unit_scale, gs.custom_frame_rate);
        }

        // Merge external animation clips.
        if (!anim_files.empty()) {
            PROF_SCOPE("fbx import (animations)");
            int added = 0;
            for (const auto& af : anim_files) {
                FBXImporter  aimp;
                FBXImportResult ar = aimp.import_file(af.string());
                if (!ar.success) {
                    std::fprintf(stderr, "  skip %s: %s\n",
                                 af.filename().string().c_str(), ar.error_message.c_str());
                    continue;
                }
                for (const auto& c : ar.clips) {
                    AnimationClipDescriptor remapped =
                        remap_clip_to_base(c, ar.skeleton, result.skeleton);
                    if (!remapped.channels.empty()) {
                        if (remapped.name.empty() || remapped.name == "clip") {
                            remapped.name = af.stem().string();
                        }
                        result.clips.push_back(std::move(remapped));
                        ++added;
                    }
                }
            }
            std::printf("External animations: %d clips added\n", added);
        }

        {
            PROF_SCOPE("pack mesh");
            mesh = pack_mesh_from_fbx(result);
        }
        if (mesh.vertices.empty()) {
            std::fprintf(stderr, "No renderable mesh in FBX.\n");
            return 1;
        }
        {
            PROF_SCOPE("resolve textures");
            resolve_submesh_textures(*result.scene, mesh);
        }
        // Material ids refer into the scene; drop them now that basenames
        // are resolved, so the cache file is scene-independent.
        for (SubMesh& sm : mesh.submeshes) sm.material_id = 0;

        skeleton = std::move(result.skeleton);
        clips    = std::move(result.clips);

        {
            PROF_SCOPE("cache write");
            if (cache::write(cache_path, mesh, skeleton, clips, sources)) {
                std::printf("Cache written: %s\n", cache_path.string().c_str());
            } else {
                std::fprintf(stderr, "Cache write failed: %s\n", cache_path.string().c_str());
            }
        }
    }

    std::printf("Mesh: %zu verts, %zu indices, %zu submeshes, bones=%zu, clips=%zu\n",
                mesh.vertices.size(), mesh.indices.size(), mesh.submeshes.size(),
                skeleton.bones.size(), clips.size());
    for (const SubMesh& sm : mesh.submeshes) {
        std::printf("  submesh  %-24s  idx=%u..%u  tex=%s\n",
                    sm.debug_name.c_str(),
                    sm.index_start, sm.index_start + sm.index_count,
                    sm.texture_basename.empty() ? "(fallback)" : sm.texture_basename.c_str());
    }

    // ── Bone hierarchy dump (diagnostic) ─────────────────────
    {
        std::vector<uint32_t> child_count(skeleton.bones.size(), 0);
        std::vector<uint32_t> root_ids;
        float max_t2 = 0.0f;
        int   max_t_bone = -1;
        for (size_t i = 0; i < skeleton.bones.size(); ++i) {
            const Bone& b = skeleton.bones[i];
            if (b.parent_index < 0) root_ids.push_back(static_cast<uint32_t>(i));
            else if (static_cast<size_t>(b.parent_index) < skeleton.bones.size())
                ++child_count[b.parent_index];
            const float3& t = b.bind_pose.translation;
            float t2 = t.x*t.x + t.y*t.y + t.z*t.z;
            if (t2 > max_t2) { max_t2 = t2; max_t_bone = static_cast<int>(i); }
        }
        std::printf("Bones: %zu total, %zu root(s), farthest-from-origin bind T=%.2f m (%s)\n",
                    skeleton.bones.size(), root_ids.size(),
                    std::sqrt(max_t2),
                    (max_t_bone >= 0 ? skeleton.bones[max_t_bone].name.c_str() : "-"));
        for (uint32_t r : root_ids) {
            const Bone& rb = skeleton.bones[r];
            std::printf("  root: [%u] %-28s  T=(%+.3f,%+.3f,%+.3f) R=(%+.4f,%+.4f,%+.4f,%+.4f) children=%u\n",
                        r, rb.name.c_str(),
                        rb.bind_pose.translation.x, rb.bind_pose.translation.y, rb.bind_pose.translation.z,
                        rb.bind_pose.rotation.x, rb.bind_pose.rotation.y,
                        rb.bind_pose.rotation.z, rb.bind_pose.rotation.w,
                        child_count[r]);
        }
        // Dump first 32 bones fully, then a summary line for the rest.
        size_t dump_n = std::min<size_t>(32, skeleton.bones.size());
        std::printf("First %zu bones (R = quat xyzw):\n", dump_n);
        for (size_t i = 0; i < dump_n; ++i) {
            const Bone& b = skeleton.bones[i];
            const char* parent_name = "(root)";
            if (b.parent_index >= 0 && static_cast<size_t>(b.parent_index) < skeleton.bones.size())
                parent_name = skeleton.bones[b.parent_index].name.c_str();
            std::printf("  [%3zu] p=%-3d  %-28s  T=(%+8.3f,%+8.3f,%+8.3f)  R=(%+.3f,%+.3f,%+.3f,%+.3f)  parent=%s\n",
                        i, b.parent_index, b.name.c_str(),
                        b.bind_pose.translation.x, b.bind_pose.translation.y, b.bind_pose.translation.z,
                        b.bind_pose.rotation.x, b.bind_pose.rotation.y,
                        b.bind_pose.rotation.z, b.bind_pose.rotation.w,
                        parent_name);
        }
        if (skeleton.bones.size() > dump_n) {
            std::printf("  ... %zu more bones omitted\n", skeleton.bones.size() - dump_n);
        }
    }
    std::printf("Keys:\n");
    std::printf("  SPACE / N : next clip     P : prev clip     R : restart clip\n");
    std::printf("  B : bind-pose toggle      L : bone overlay  M : mesh toggle\n");
    std::printf("  F : fur shells            [ ] shells -/+   - = length   9 0 density\n");
    std::printf("  , . tip thickness         O : root occlusion cycle\n");
    std::printf("  T : rope binding          Y : pattern cycle  I/K : tightness -/+  H : rope tube\n");
    std::printf("  J : toon tears\n");
    std::printf("  Bone colors — grey = bind pose, green = current pose,\n");
    std::printf("                red/green/blue axes at world origin (1m each).\n");

    Profiler::instance().print("Load profile");

    FBXViewer viewer;
    if (!viewer.initialize(std::move(mesh), std::move(skeleton), std::move(clips),
                           model_dir, shader_dir, options)) {
        std::fprintf(stderr, "Viewer init failed.\n");
        return 1;
    }
    viewer.run();
    viewer.shutdown();
    return 0;
}

#endif // PICTOR_HAS_VULKAN
