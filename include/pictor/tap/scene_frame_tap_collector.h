#pragma once

/// SceneRegistry の可視オブジェクトをフレームタップの scene pass へ流す
/// (spec/feature/frame-tap.md §6)。
///
/// 対象は static / dynamic プールのうちカリングで可視になった物 — BatchBuilder が
/// バッチにする集合と同じ。 GPU-driven プールは GPU 側でカリングするため CPU に
/// 可視集合が無く、 読み戻しをしない原則により対象外。
///
/// 可視性の根拠は FrameTap::visibility_source() があればそれ、 無ければ frustum-only。

#include "pictor/scene/scene_registry.h"
#include "pictor/tap/frame_tap.h"

namespace pictor {

/// カリング後 (render() の Culling 以降) に呼ぶ。 `tap` が開いたフレームを持つこと。
void collect_scene_frame_tap(const SceneRegistry& scene, FrameTap& tap);

} // namespace pictor
