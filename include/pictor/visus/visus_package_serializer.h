#pragma once

#include "pictor/visus/visus_shader_package.h"

#include <string>
#include <vector>

namespace pictor {

// ============================================================
// シェーダーパッケージのシリアライザ (§2.5.1)
// ============================================================
// `<name>.shaderpkg.json` (version 1):
//   {
//     "version": 1,
//     "name":    "toon",
//     "shader":  "builtin:toon",           // §2.3 の 3 形式
//     "params":  { "rim_power": 2.0 },     // 動的パラメータの既定値
//     "metadata": { "shader.key_override": 3 }
//   }
//
// visus_serializer と同じ手書きパーサ (外部依存なし)。 未知 key は
// params / metadata 内なら保持、 トップレベルなら無視する。

/// Visus 側の参照 (文字列 "toon" または object) → VisusPackageRef。
/// package 名が取れなければ false。
bool visus_package_ref_from_value(const VisusValue&         value,
                                  VisusPackageRef&          out,
                                  std::vector<std::string>* warnings = nullptr);

/// VisusPackageRef → JSON 値。 上書きが無ければ短い文字列形式で出す。
VisusValue visus_package_ref_to_value(const VisusPackageRef& ref);

/// パッケージ本体 ↔ JSON 値。
VisusValue visus_package_to_value(const VisusShaderPackage& pkg);
bool       visus_package_from_value(const VisusValue&         value,
                                    VisusShaderPackage&       out,
                                    std::string*              error    = nullptr,
                                    std::vector<std::string>* warnings = nullptr);

/// パッケージ本体 ↔ JSON テキスト。
std::string to_shader_package_json(const VisusShaderPackage& pkg);
bool        from_shader_package_json(const std::string&        json,
                                     VisusShaderPackage&       out,
                                     std::string*              error    = nullptr,
                                     std::vector<std::string>* warnings = nullptr);

} // namespace pictor
