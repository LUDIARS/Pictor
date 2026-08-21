#pragma once

#include "pictor/visus/visus.h"
#include "pictor/core/types.h"

namespace pictor {

// VisusDesc の metadata から ObjectDescriptor の共通フィールドを作る。
// flags / lod は VisusDesc の
// metadata (`render.flags` / `render.layer` / `render.lod`、 無ければ v1
// 既定値 DYNAMIC / 0 / 0) から複写する。 `shader.key_override` は
// shaderKey 下位ビットへ。
//
// Visus v2 は JSON に handle を持たないため、 この helper は mesh / material /
// customShader を解決しない。 SceneRegistry への登録を伴う instantiate API は
// VisusRuntime と同時に task 2 (`visus-v2-design.md` §3.2) で導入する。

/// VisusDesc の metadata から ObjectDescriptor の共通部 (flags / lod /
/// shaderKey) を組み立てる。
ObjectDescriptor visus_base_descriptor(const VisusDesc& desc,
                                       const float4x4&  transform,
                                       const AABB&      bounds);

} // namespace pictor
