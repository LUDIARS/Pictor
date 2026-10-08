#include "morph_loader.h"

#include <cstdio>

namespace pictor_fbx_viewer {

/// @implements SPEC-PC-FBX-TRACK-PLAYBACK
MorphTargets build_morph_targets(const pictor::FBXImportResult& result, const PackedMesh& mesh) {
    MorphTargetsBuilder builder;
    if (!result.scene || mesh.vertices.empty()) return builder.build();
    const pictor::FBXScene& scene = *result.scene;
    const float* positions = mesh.vertices.front().position;
    constexpr size_t kStride = sizeof(TexturedSkinnedVertex) / sizeof(float);

    size_t   skin_index  = 0;
    uint32_t vertex_base = 0;
    for (pictor::FBXObjectId gid : scene.ids_of_type(pictor::FBXObjectType::GEOMETRY)) {
        // Same filter as FBXImporter::build_skin_meshes ...
        const pictor::FBXObject* g = scene.get(gid);
        if (!g || !g->geometry) continue;
        const pictor::FBXGeometry::Triangulated* tri = scene.triangulate(gid);
        if (!tri || !tri->valid) continue;
        if (skin_index >= result.skin_meshes.size()) break;
        const pictor::SkinMeshDescriptor& sm = result.skin_meshes[skin_index++];
        // ... and as pack_one_geometry, which also drops empty geometries.
        if (tri->positions.empty() || tri->indices.empty()) continue;
        const uint32_t vcount = static_cast<uint32_t>(tri->positions.size());
        if (sm.vertex_count != vcount || vertex_base + vcount > mesh.vertices.size()) {
            std::fprintf(stderr, "[morph] geometry layout mismatch at '%s'; blendshapes disabled\n",
                         sm.name.c_str());
            return MorphTargetsBuilder{}.build();
        }
        const size_t per_shape = static_cast<size_t>(vcount) * 3;
        for (size_t k = 0; k < sm.morph_target_names.size(); ++k) {
            if ((k + 1) * per_shape > sm.morph_deltas.size()) break;
            builder.add_shape(sm.morph_target_names[k], vertex_base, vcount,
                              sm.morph_deltas.data() + k * per_shape, positions, kStride);
        }
        vertex_base += vcount;
    }
    if (vertex_base != mesh.vertices.size()) {
        std::fprintf(stderr, "[morph] packed %u of %zu vertices; blendshapes disabled\n",
                     vertex_base, mesh.vertices.size());
        return MorphTargetsBuilder{}.build();
    }
    return builder.build();
}

} // namespace pictor_fbx_viewer
