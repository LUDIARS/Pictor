#pragma once

#include "pictor/visus/visus.h"
#include "pictor/scene/scene_registry.h"

#include <vector>

namespace pictor {

// ============================================================
// instantiate_visus — VisusDesc + transform/bounds → ObjectId[]
// ============================================================
// 1 Visus = N ObjectDescriptor (N = max(materials.size(), 1))。
//
//   - materials が空     : 1 個だけ INVALID_MATERIAL で登録
//   - materials が N > 0 : slot ごとに 1 個ずつ ObjectDescriptor を登録
//
// 各 ObjectDescriptor の共通フィールド (mesh / transform / bounds /
// flags / lod) は VisusDesc から複写、 material と materialKey は slot
// 単位で差し込む。
//
// 親子・コンポジションは作らない (Ergo 側ノード表現の責務)。
//
// 戻り値: 生成された ObjectId のリスト (slot 順)。 SceneRegistry への
// 登録は完了済。

std::vector<ObjectId> instantiate_visus(
    SceneRegistry&     scene,
    const VisusDesc&   desc,
    const float4x4&    transform,
    const AABB&        bounds);

} // namespace pictor
