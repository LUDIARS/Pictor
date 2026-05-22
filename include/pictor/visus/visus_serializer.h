#pragma once

#include "pictor/visus/visus.h"

#include <string>

namespace pictor {

// ============================================================
// Visus serializer — VisusDesc ↔ JSON
// ============================================================
// material_serializer.h と同じ手書きパーサ方針。 外部依存なし。
//
// JSON layout (version 1):
//   {
//     "version":   1,
//     "name":      "kuzu_enemy_mob",
//     "geometry": {
//       "kind":           "model",         // none|primitive|model|rive|ui|particle|text|custom
//       "asset": {
//         "local_path":   "models/kuzu.fbx",
//         "remote_url":   "https://cdn.example/kuzu.fbx",
//         "sha256":       "deadbeef...",
//         "size_bytes":   12345678,
//         "fetch_policy": "cache_first",   // local_only|cache_first|revalidate|always_fetch
//         "headers": [ { "name": "X-Auth", "value": "${env:CDN_TOKEN}" } ]
//       },
//       "shader_stages": {                   // CUSTOM kind: vert/frag/comp
//         "vert": { /* ResourceRef */ },
//         "frag": { /* ResourceRef */ },
//         "comp": { /* ResourceRef */ },
//         "vertex_layout": {                 // §6.2 mesh 駆動の頂点入力
//           "stride": 64,                    //   0 = attribute から推定
//           "attributes": [                  //   空配列 = gl_VertexIndex 駆動
//             { "semantic": "position", "type": "float3",   "offset": 0  },
//             { "semantic": "joints",   "type": "uint32x4", "offset": 32 }
//           ]
//         }
//       },
//       "rive_artboard": "",                // RIVE only
//       "text_default":  "",                // TEXT only
//
//       "mesh":           "handle:0|none",  // PRIMITIVE 解決済 handle
//       "model":          "handle:5",       // MODEL 解決済 handle
//       "shader":         "none",           // CUSTOM 解決済
//       "generic_handle": 0                  // RIVE/UI/PARTICLE/TEXT 解決済
//     },
//     "materials": [
//       { "slot": "body",  "material": "handle:12",
//         "resource": { /* ResourceRef */ } },
//       { "slot": "armor", "material": "handle:13",
//         "resource": { /* ResourceRef */ } }
//     ],
//     "textures": [
//       { "slot": "albedo_override", "texture": "handle:7",
//         "resource": { /* ResourceRef */ } }
//     ],
//     "flags": {
//       "default_flags": 2,        // ObjectFlags::* bitmask
//       "layer":         0,
//       "pool_hint":     "dynamic",
//       "initial_lod":   0
//     },
//     "animation_default": {
//       "kind":  "clip",            // none|clip|state_machine|rive_animation|rive_state_machine
//       "name":  "idle",
//       "loop":  true,
//       "speed": 1.0
//     },
//     "shader_key_override": 0
//   }
//
// 解釈: 未知の key は黙ってスキップ。 構文エラーのみ false。

/// Serialize `desc` to canonical pretty-printed JSON.
std::string to_visus_json(const VisusDesc& desc);

/// Parse JSON to `out`. 構文エラー時のみ false、 内容欠落は default。
/// `error` (non-null) に短い理由を入れる。
bool from_visus_json(const std::string& json,
                     VisusDesc&         out,
                     std::string*       error = nullptr);

} // namespace pictor
