#pragma once

#include "pictor/core/types.h"
#include "pictor/scene/scene_registry.h"
#include "pictor/visus/visus.h"
#include "pictor/visus/visus_catalog.h"
#include "pictor/visus/visus_runtime.h"

#include <string>
#include <string_view>
#include <vector>

namespace pictor {

// ============================================================
// instantiate_visus — Visus (name) + transform/bounds → ObjectDescriptor 群
// ============================================================
// `spec/feature/visus-v2-design.md` §2.4 / §3.2。
//
//   kind=model     : resolver が列挙した実 draw part ごとに 1 ObjectDescriptor。
//                    parts[] は exact / "*" の shader 設定として適用する。
//                    shaderKey / customShader / material は VisusRuntime の
//                    解決済み part から。 未列挙パーツは "*"、 無ければ既定 PBR。
//   kind=primitive : 解決済み MeshHandle があれば 1 個
//   kind=custom    : 0 個 (model part から参照する shader 定義)
//   kind=rive/ui/particle/text : 0 個。 generic_handle をホストが具現化する
//   kind=group/none: 0 個 (children のみ)
//
//   children       : 再帰。 親が kind=model かつ attach.bone 指定なら
//                    親 transform × bone (resolver.bone_transform) × offset、
//                    それ以外は親 transform (警告)。 深さ 8 超 / 循環は打ち切り。
//
// 共通フィールド (flags / layer / lod / shaderKey 下位) は metadata
// (`render.flags` / `render.layer` / `render.lod` / `shader.key_override`) から。
// `render.pool` は ObjectDescriptor に対応フィールドが無いので読まない。
//
// 子 instance は親と独立 (親の ObjectId 群とは別)。 親を消すとき子も消す
// 責務はホスト側。
//
// `bounds` は Visus 木の全 object にそのまま複写する。 Pictor は mesh の
// 実 AABB を持たない (資源はホスト所有) ため、 呼び出し側が木全体を覆う
// bounds を渡す契約。 bone アタッチした子は親から動くので、 タイトな
// per-object bounds が要るホストは instantiate 後に自分で上書きする。

struct VisusInstance {
    std::string                name;
    float4x4                   transform = float4x4::identity();
    std::vector<ObjectId>      objects;    // この Visus 自身の ObjectDescriptor (part 順)
    std::vector<VisusInstance> children;   // children 順
    uint32_t                   generic_handle = 0; // RIVE / UI / PARTICLE / TEXT

    /// 自身 + 子孫の ObjectId を深さ優先で集める (破棄用)。
    void collect_objects(std::vector<ObjectId>& out) const;
};

/// VisusDesc の metadata から ObjectDescriptor の共通部 (flags / lod /
/// shaderKey 下位ビット) を組み立てる。 instantiate_visus の部品。
ObjectDescriptor visus_base_descriptor(const VisusDesc& desc,
                                       const float4x4&  transform,
                                       const AABB&      bounds);

/// 行優先 (translation = m[3][*]) の float4x4 合成: `local` を `parent` 空間へ
/// (= local × parent)。 children の transform 合成に使う。 公開はテスト用。
float4x4 visus_compose(const float4x4& local, const float4x4& parent);

/// `name` を instantiate して SceneRegistry に登録する。 runtime に解決結果が
/// 無い Visus は「資源無し」のまま登録する (mesh / shader は INVALID)。
/// `resolver` (任意) は children の bone 行列取得にだけ使う。
/// 戻り値: root が catalog に存在し登録できたか。 子の失敗は warnings。
bool instantiate_visus(SceneRegistry&            scene,
                       const VisusCatalog&       catalog,
                       const VisusRuntime&       runtime,
                       std::string_view          name,
                       const float4x4&           transform,
                       const AABB&               bounds,
                       VisusInstance&            out,
                       std::vector<std::string>* warnings = nullptr,
                       IVisusResolver*           resolver = nullptr);

} // namespace pictor
