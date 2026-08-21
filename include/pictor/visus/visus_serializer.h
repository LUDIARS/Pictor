#pragma once

#include "pictor/visus/visus.h"

#include <string>
#include <vector>

namespace pictor {

// ============================================================
// Visus serializer — VisusDesc ↔ JSON (version 2)
// ============================================================
// 外部依存なしの手書きパーサ (`src/visus/visus_json.*`)。
//
// JSON layout (version 2、 `spec/feature/visus-v2-design.md` §2.1):
//   {
//     "version": 2,
//     "name":    "kuzuha",                 // 唯一の identity
//     "kind":    "model",                  // none|model|rive|primitive|custom|ui|particle|text|group
//     "asset":   "../../models/kuzuha.fbx",// visus ファイル起点の相対 or 絶対
//     "parts": [                           // kind=model: fbx 内パーツ → シェーダ
//       { "part": "T_Cloak_bsc", "shader": "builtin:pbr",
//         "metadata": { "texture.diffuse": "../tex/T_Cloak_bsc.png" } },
//       { "part": "T_Face_bsc",
//         "shader": { "vert": "../shaders/face.vert.spv", "frag": "../shaders/face.frag.spv" } }
//     ],
//     "children": [                        // 入れ子 Visus (名前参照)
//       { "visus": "kuzuha_facial",
//         "attach": { "bone": "Head", "offset": [0, 0.02, 0.05] },
//         "metadata": { "layer": "overlay" } }
//     ],
//     "metadata": {                        // Pictor は解釈しない (規約 key は visus_keys)
//       "animation.default": "Idle", "animation.loop": true,
//       "render.flags": 2, "scale.target_height": 1.6
//     }
//   }
//
// 解釈: 未知 key は `metadata` 内なら保持、 トップレベルなら `metadata` へ
// 吸収せず無視する。 構文エラー / 未対応 version は false。
//
// version 1 (旧 typed スキーマ) は読込時に v2 へ変換する (§2.2 の表)。
// 解決済み handle ("handle:N") は捨てる。 変換時は `warnings` に
// "v1 converted" を積む。 書き出しは常に version 2。
//
// `version` key 自体が無い文書は原則 version 2 として読むが、 v1 固有の
// トップレベル key (`geometry` / `materials` / `textures` / `flags` /
// `animation_default` / `shader_key_override`) があれば v1 として変換し、
// `warnings` に "visus version missing" を積む。 これが無いと version を
// 落とした v1 文書が `name` だけ残して黙って成功する。

/// Serialize `desc` to canonical pretty-printed JSON (version 2).
std::string to_visus_json(const VisusDesc& desc);

/// Parse JSON (version 1 or 2) to `out`. 構文エラー / 未対応 version は false、
/// 内容欠落は default。 `error` (non-null) に短い理由、 `warnings` (non-null) に
/// 非致命の注意 (v1 変換・未対応 shader 参照など) を積む。
bool from_visus_json(const std::string&        json,
                     VisusDesc&                out,
                     std::string*              error    = nullptr,
                     std::vector<std::string>* warnings = nullptr);

/// 文書ルートの実効 version を返す。 `version` key が無い文書は v1 固有の
/// トップレベル key の有無で 1.0 / 2.0 を判定する (欠落 v1 を v2 と誤認しない
/// ため、 読込経路と migrate 経路は必ずこの判定を共有する)。 `version` が数値
/// でなければ false。
bool visus_document_version(const VisusMetadata&      root,
                            double&                   out_version,
                            std::string*              error    = nullptr,
                            std::vector<std::string>* warnings = nullptr);

/// VisusDesc → 汎用 JSON 値 (version 2 の構造)。 エディタ / migrate 用。
VisusValue visus_desc_to_value(const VisusDesc& desc);

/// 汎用 JSON 値 (version 2 の構造) → VisusDesc。 `version` が 1 なら v1
/// 変換経路に回す。 構造が object でなければ false。
bool visus_desc_from_value(const VisusValue&         value,
                           VisusDesc&                out,
                           std::string*              error    = nullptr,
                           std::vector<std::string>* warnings = nullptr);

} // namespace pictor
