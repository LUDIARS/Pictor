#pragma once

#include "pictor/visus/visus.h"
#include "pictor/visus/visus_metadata.h"

#include <string>
#include <string_view>
#include <vector>

namespace pictor {

// ============================================================
// VisusShaderPackage — シェーダ + 動的パラメータの束 (§2.5)
// ============================================================
// `spec/feature/visus-v2-design.md` §2.5。
//
// neco 方針: 「メタデータはマテリアルの上位層。 シェーダとゲーム中動的に
// 変えるパラメータのペアをシェーダーパッケージとして Visus にアサインする。
// 複数アサインできる」。
//
//   shader   : §2.3 のシェーダ参照 (builtin: / stages / visus:)
//   params   : ★ゲーム中に変える値の既定値 (uniform 名 / texture.<slot> 等)
//   metadata : ★変えない pipeline 設定 (shader.vertex_layout / key_override 等)
//
// params と metadata の線引きは「ゲーム中に変わるか」。 Pictor はどちらの
// 中身も解釈せず、 key 規約だけ文書化してホストへ委ねる (§2.2 と同じ)。
//
// パッケージは独立ファイル (`<name>.shaderpkg.json`) に置く再利用資源で、
// Visus からは name 参照のみ (インライン定義は許さない)。

struct VisusShaderPackage {
    std::string    name;       // 唯一の identity。 ファイル名と一致させる
    VisusShaderRef shader;
    VisusMetadata  params;     // 動的パラメータの既定値
    VisusMetadata  metadata;   // pipeline 設定

    bool empty() const { return name.empty(); }
};

// VisusPackageRef (Visus / part からのアサイン) は `visus.h` にある
// (VisusDesc / VisusPart が直接持つため)。

// ============================================================
// マージ (§2.5.3)
// ============================================================

/// `over` の全 key を `base` へ上書きコピーする (未指定 key は base のまま)。
void visus_metadata_merge(VisusMetadata& base, const VisusMetadata& over);

/// 参照 `over` を `base` へ重ねる: enabled は上書き、 params / metadata は
/// key 単位マージ。 package 名は変わらない。
void visus_package_ref_merge(VisusPackageRef& base, const VisusPackageRef& over);

/// Visus 直下と part の参照列を §2.5.3 の規則でマージした実効列。
///   1. visus_level を配列順に並べる
///   2. part_level を順に見て、 同名は「その位置のまま」上書きマージ、
///      無ければ末尾へ追加
///   3. enabled == false を除外
/// 同じ列の中で package 名が重複しているときは先勝ち + 後続はマージ扱い。
std::vector<VisusPackageRef> visus_effective_packages(
    const std::vector<VisusPackageRef>& visus_level,
    const std::vector<VisusPackageRef>& part_level);

/// パッケージ既定値 `pkg.params` に参照の上書きを重ねた実パラメータ。
VisusMetadata visus_package_effective_params(const VisusShaderPackage& pkg,
                                             const VisusPackageRef&    ref);

/// `pkg.metadata` に参照の上書きを重ねた実 pipeline 設定。
VisusMetadata visus_package_effective_metadata(const VisusShaderPackage& pkg,
                                               const VisusPackageRef&    ref);

} // namespace pictor
