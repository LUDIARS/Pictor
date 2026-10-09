#include "pictor/tap/scene_frame_tap_collector.h"

namespace pictor {

namespace {

void collect_pool(const ObjectPool& pool, FrameTap& tap,
                  const IFrameTapVisibilitySource* visibility_source) {
    const uint32_t        count      = pool.count();
    const uint8_t*        visibility = pool.visibility_flags().data();
    const MeshHandle*     meshes     = pool.mesh_handles().data();
    const MaterialHandle* materials  = pool.material_handles().data();
    const ObjectId*       ids        = pool.object_ids().data();
    const float4x4*       transforms = pool.transforms().data();
    const AABB*           bounds     = pool.bounds().data();

    for (uint32_t i = 0; i < count; ++i) {
        if (visibility[i] == 0) continue;
        FrameTapSceneObject object;
        object.mesh         = meshes[i];
        object.material     = materials[i];
        object.object       = ids[i];
        object.world        = transforms[i];
        object.world_bounds = bounds[i];
        // CPU カリングは視錐台判定だけ。 遮蔽の根拠はホストの口があるときだけ使う。
        object.visibility   = visibility_source ? visibility_source->visibility(ids[i])
                                                : FrameTapVisibility::FRUSTUM_ONLY;
        tap.add_scene_draw(object);
    }
}

} // namespace

void collect_scene_frame_tap(const SceneRegistry& scene, FrameTap& tap) {
    if (!tap.is_enabled()) return;
    const IFrameTapVisibilitySource* visibility_source = tap.visibility_source();
    collect_pool(scene.static_pool(), tap, visibility_source);
    collect_pool(scene.dynamic_pool(), tap, visibility_source);
}

} // namespace pictor
