#pragma once

#include "pictor/core/types.h"
#include "pictor/data/model_data_types.h"   // ModelHandle
#include "pictor/visus/visus.h"
#include "pictor/visus/visus_catalog.h"
#include "pictor/visus/visus_package_catalog.h"
#include "pictor/visus/visus_shader_package.h"

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace pictor {

// ============================================================
// VisusRuntime — name → 解決済み資源の side-table
// ============================================================
// `spec/feature/visus-v2-design.md` §3.2。 Visus v2 の JSON は handle を
// 持たない (同一性は name のみ)。 実行時に必要な ModelHandle /
// ShaderHandle / part 別 shaderKey はここにだけ置き、 ファイルには出さない。
//
// 資源の実ロードは Pictor が持たない (上位非依存)。 ホストが
// `IVisusResolver` を実装して注入し、 `VisusRuntime::resolve` が
// カタログを辿って side-table を埋める。 未実装の resolver メソッドは
// INVALID を返すので、 その Visus は「解決済みだが描画資源なし」になる。

// ============================================================
// IVisusResolver — ホストが実装する資源ロード窓口
// ============================================================
// 受け取るパスは VisusCatalog::resolve_path 済み (visus ファイル起点を
// 解決した generic パス) だが、asset root 内への containment は保証しない。
// JSON を信頼しないホストは resolver で allowlisted root / asset manifest / 署名を
// 検証すること。特に SPIR-V は GPU executable input なので trusted asset のみ許可する。
// 既定実装はすべて「未対応 = INVALID / false」。

struct VisusModelPart {
    std::string name;                         // fbx 内の draw part / material slot 名
    MeshHandle  mesh = INVALID_MESH;          // SceneRegistry が描画する実 mesh
};

class IVisusResolver {
public:
    virtual ~IVisusResolver() = default;

    /// kind=primitive の mesh。
    virtual MeshHandle load_mesh(const std::string& path, std::string* error) {
        (void)path;
        if (error) *error = "mesh loading is not supported";
        return INVALID_MESH;
    }

    /// kind=model の fbx 等。
    virtual ModelHandle load_model(const std::string& path, std::string* error) {
        (void)path;
        if (error) *error = "model loading is not supported";
        return INVALID_MODEL;
    }

    /// `load_model` が返した model の実 draw part 一覧。 Visus の `parts[]` は
    /// shader 設定であって geometry 一覧ではないため、 runtime はこの結果へ
    /// exact / "*" 設定を適用する。 mesh が INVALID の項目は描画対象外。
    virtual std::vector<VisusModelPart> model_parts(ModelHandle model,
                                                    std::string* error) {
        (void)model;
        if (error) *error = "model part enumeration is not supported";
        return {};
    }

    /// SPIR-V 直指定 (VisusShaderRef::Kind::STAGES)。 vert/frag/comp は解決済みパス、
    /// `metadata` は参照元 (part または kind=custom の Visus) の metadata
    /// (`shader.vertex_layout` 等)。
    virtual ShaderHandle register_shader_stages(const VisusShaderRef& stages,
                                                const VisusMetadata&  metadata,
                                                std::string*          error) {
        (void)stages; (void)metadata;
        if (error) *error = "shader stage registration is not supported";
        return INVALID_SHADER;
    }

    /// "builtin:<name>"。 INVALID_SHADER = 既定 PBR 経路 (shaderKey にカスタム
    /// ビットを立てない)。 "pbr" 以外を特別扱いしたいホストが override する。
    virtual ShaderHandle builtin_shader(std::string_view name) {
        (void)name;
        return INVALID_SHADER;
    }

    /// kind=rive / ui / particle / text の汎用 handle。 0 = 未解決。
    virtual uint32_t load_generic(VisusKind kind, const std::string& path, std::string* error) {
        (void)kind; (void)path;
        if (error) *error = "generic asset loading is not supported";
        return 0;
    }

    /// model part metadata の `material`、 および kind=primitive の visus
    /// metadata の `material` (material JSON パス)。 slot 付き
    /// `material.<slot>` は v2 では受け皿が無く、 ホスト解釈の metadata のまま。
    virtual MaterialHandle load_material(const std::string& path, std::string* error) {
        (void)path;
        if (error) *error = "material loading is not supported";
        return INVALID_MATERIAL;
    }

    /// children の `attach.bone` 用: model の bone のワールド (モデル空間)
    /// 行列。 false = 不明 (instantiate は親 transform をそのまま使う)。
    virtual bool bone_transform(ModelHandle model, std::string_view bone, float4x4& out) {
        (void)model; (void)bone; (void)out;
        return false;
    }
};

// ============================================================
// 解決結果
// ============================================================

// ============================================================
// VisusResolvedPackage — アサインされたシェーダーパッケージの解決結果 (§2.5.4)
// ============================================================
// `params` は「パッケージ既定値 + アサイン時の上書き」をマージ済みの実値で、
// instantiate 後にホストが書き換える動的パラメータの初期値になる
// (`VisusInstance::set_param`)。 `metadata` は pipeline 設定 (静的)。

struct VisusResolvedPackage {
    std::string   package;
    ShaderHandle  shader     = INVALID_SHADER;
    uint64_t      shader_key = 0;
    VisusMetadata params;
    VisusMetadata metadata;
};

struct VisusResolvedPart {
    std::string    part;                          // resolver が列挙した実 draw part 名
    MeshHandle     mesh       = INVALID_MESH;
    ShaderHandle   shader     = INVALID_SHADER;   // STAGES / VISUS / builtin 非既定
    MaterialHandle material   = INVALID_MATERIAL;
    uint64_t       shader_key = 0;                // key_override + custom ビット
    std::vector<VisusResolvedPackage> packages;   // base の上に重ね掛けする実効列 (§2.5.3)
};

struct VisusResolved {
    std::string  name;
    VisusKind    kind           = VisusKind::NONE;
    MeshHandle   mesh           = INVALID_MESH;     // PRIMITIVE
    ModelHandle  model          = INVALID_MODEL;    // MODEL
    ShaderHandle shader         = INVALID_SHADER;   // CUSTOM (metadata["shader"])
    MaterialHandle material     = INVALID_MATERIAL; // PRIMITIVE (metadata["material"])
    uint32_t     generic_handle = 0;                // RIVE / UI / PARTICLE / TEXT
    uint64_t     shader_key     = 0;                // CUSTOM / 非 model の shaderKey
    std::vector<VisusResolvedPart> parts;           // MODEL: resolver の draw part 順
    std::vector<VisusResolvedPackage> packages;     // 非 MODEL: Visus 直下のアサイン

    const VisusResolvedPart* find_part(std::string_view part) const;
};

// ============================================================
// VisusRuntime
// ============================================================

class VisusRuntime {
public:
    VisusRuntime() = default;

    /// `name` とその children を再帰的に解決して side-table を埋める。 既に
    /// 解決済みの name は飛ばす (再解決は `invalidate` 後)。 未解決 child /
    /// 循環 / 深さ超過 / resolver エラーは `warnings` に積み、 戻り値は
    /// 「root が解決できたか」。
    /// `packages` (任意) を渡すと Visus / part にアサインされたシェーダー
    /// パッケージ (§2.5) も解決する。 nullptr のとき packages を持つ Visus は
    /// warnings に「カタログ未指定」を積み、 base シェーダだけで解決する。
    bool resolve(const VisusCatalog&        catalog,
                 std::string_view           name,
                 IVisusResolver&            resolver,
                 std::vector<std::string>*  warnings = nullptr,
                 const VisusPackageCatalog* packages = nullptr);

    /// 手組みの解決結果を登録 (テスト / ホスト独自経路)。 同名は上書き。
    void set(VisusResolved resolved);

    const VisusResolved* get(std::string_view name) const;
    bool                 is_resolved(std::string_view name) const { return get(name) != nullptr; }

    /// `name` が解決済みなら side-table 全体を無効化する。 children と
    /// `visus:` shader は entries 間で共有され、依存元を個別 entry からは
    /// 復元できないため、部分無効化は stale handle を残す。
    bool invalidate(std::string_view name);
    void clear() { resolved_.clear(); }
    size_t size() const { return resolved_.size(); }

private:
    bool resolve_one_(const VisusCatalog& catalog, const VisusDesc& desc,
                      IVisusResolver& resolver, std::vector<std::string>* warnings,
                      const VisusPackageCatalog* packages);
    void resolve_model_(const VisusCatalog& catalog, const VisusDesc& desc,
                        const std::string& asset, IVisusResolver& resolver,
                        VisusResolved& out,
                        std::vector<std::string>* warnings,
                        const VisusPackageCatalog* packages);
    bool resolve_rec_(const VisusCatalog& catalog, const VisusDesc& desc,
                      IVisusResolver& resolver, int depth,
                      std::vector<std::string>& stack,
                      std::map<std::string, uint16_t, std::less<>>& visited_depths,
                      std::vector<std::string>* warnings,
                      const VisusPackageCatalog* packages);
    /// 実効列 (§2.5.3) を解決して `out` へ積む。
    void resolve_packages_(const VisusCatalog& catalog, const VisusDesc& desc,
                           const std::vector<VisusPackageRef>& part_level,
                           IVisusResolver& resolver,
                           const VisusPackageCatalog* packages,
                           std::vector<VisusResolvedPackage>& out,
                           std::vector<std::string>* warnings);
    ShaderHandle resolve_package_shader_(const VisusCatalog& catalog, const VisusDesc& owner,
                                         const VisusPackageCatalog& packages,
                                         const VisusShaderPackage& pkg,
                                         const VisusMetadata& metadata,
                                         IVisusResolver& resolver,
                                         std::vector<std::string>* warnings);
    ShaderHandle resolve_shader_ref_(const VisusCatalog& catalog, const VisusDesc& owner,
                                     const VisusShaderRef& ref, const VisusMetadata& metadata,
                                     IVisusResolver& resolver, int depth,
                                     std::vector<std::string>* warnings);

    std::map<std::string, VisusResolved, std::less<>> resolved_;
};

} // namespace pictor
