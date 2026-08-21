#pragma once

#include "pictor/visus/visus_metadata.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace pictor {

// ============================================================
// Visus v2 — メタデータ駆動の描画定義
// ============================================================
// `spec/feature/visus-v2-design.md`。
//
// Visus は「ファイル上の定義」であり、 Pictor 内の handle や上位
// (Ergo / ゲーム) 側のオブジェクトと同一性を保証できない。 よって
// Visus が持つ identity は `name` のみ。 それ以外の値は `metadata`
// (key → JSON 値) として保持し、 解釈はホストに委ねる。 Pictor は
// 規約 key (§2.2) を文書化するだけで構造を強制しない。
//
// typed フィールドは name / kind / asset / parts / children / metadata
// の 6 つ。 解決済み handle は JSON に書かず、 実行時 side-table
// (VisusRuntime、 task 2) にのみ置く。

// ============================================================
// VisusKind
// ============================================================

enum class VisusKind : uint8_t {
    NONE      = 0,
    MODEL     = 1,  // fbx 等 (skin mesh + rig + clips)。 parts[] でパーツ別シェーダ
    RIVE      = 2,  // .riv
    PRIMITIVE = 3,  // 単一 mesh
    CUSTOM    = 4,  // カスタムシェーダ定義 (metadata["shader"])
    UI        = 5,
    PARTICLE  = 6,
    TEXT      = 7,
    GROUP     = 8   // asset 無し、 children だけを束ねる
};

const char* visus_kind_to_str(VisusKind k);
VisusKind   visus_kind_from_str(std::string_view s);

// ============================================================
// VisusShaderRef — シェーダ参照 (§2.3)
// ============================================================
// いずれも名前かパスで、 handle は使わない。
//   BUILTIN : "builtin:<name>"   ホスト組込み pipeline (既定 "pbr")
//   STAGES  : { "vert": path, "frag": path, "comp"?: path }  SPIR-V 直指定
//   VISUS   : "visus:<name>"     kind=custom の別 Visus をシェーダ定義として参照

struct VisusShaderRef {
    enum class Kind : uint8_t { BUILTIN = 0, STAGES = 1, VISUS = 2 };

    Kind        kind = Kind::BUILTIN;
    std::string name = "pbr";   // BUILTIN / VISUS
    std::string vert;           // STAGES
    std::string frag;           // STAGES
    std::string comp;           // STAGES (任意)

    static VisusShaderRef builtin(std::string name = "pbr");
    static VisusShaderRef stages(std::string vert, std::string frag, std::string comp = {});
    static VisusShaderRef visus(std::string name);

    bool is_default_builtin() const { return kind == Kind::BUILTIN && name == "pbr"; }
    bool has_graphics_stages() const {
        return kind == Kind::STAGES && !vert.empty() && !frag.empty();
    }

    bool operator==(const VisusShaderRef& o) const {
        return kind == o.kind && name == o.name && vert == o.vert &&
               frag == o.frag && comp == o.comp;
    }
    bool operator!=(const VisusShaderRef& o) const { return !(*this == o); }
};

/// JSON 値 (string "builtin:x" / "visus:x" / object {vert,frag,comp}) →
/// VisusShaderRef。 解釈不能なら false (out は不変)。
bool visus_shader_ref_from_value(const VisusValue& v, VisusShaderRef& out);

/// VisusShaderRef → JSON 値 (上の逆)。
VisusValue visus_shader_ref_to_value(const VisusShaderRef& ref);

// ============================================================
// VisusPackageRef — シェーダーパッケージのアサイン (§2.5.2)
// ============================================================
// パッケージ本体 (VisusShaderPackage、 `visus_shader_package.h`) は
// `<name>.shaderpkg.json` に置く再利用資源で、 Visus からは name 参照
// のみ (インライン定義は許さない)。 Visus 直下の列は全パーツへ重ね掛け、
// parts[] の列はその後ろに append される (マージ規則は §2.5.3 /
// `visus_effective_packages`)。
//
// JSON は文字列 ("toon") か object:
//   { "package": "outline", "enabled": false, "params": {}, "metadata": {} }

struct VisusPackageRef {
    std::string   package;          // パッケージ名 (必須)
    bool          enabled = true;   // false = 継承した同名アサインを無効化
    VisusMetadata params;           // 既定 params への key 単位の上書き
    VisusMetadata metadata;         // 既定 metadata への key 単位の上書き

    bool valid() const { return !package.empty(); }
};

// ============================================================
// VisusPart — kind=model の fbx 内パーツ → シェーダ
// ============================================================
// `part` は fbx 内部のパーツ名 (ホスト側の draw part 名 / material slot
// 名)。 "*" は既定 (未列挙パーツ)。

struct VisusPart {
    std::string    part;
    VisusShaderRef shader;     // base パス (§2.5.3)。 パッケージはこの後ろに重なる
    VisusMetadata  metadata;   // texture.<slot> 等
    std::vector<VisusPackageRef> packages;   // このパーツ固有のアサイン (§2.5.2)

    bool is_wildcard() const { return part == "*"; }
};

// ============================================================
// VisusAttach / VisusChildRef — 入れ子 Visus (§2.4)
// ============================================================

struct VisusAttach {
    std::string bone;              // 親 kind=model のときだけ有効
    float       offset[3] = {0.0f, 0.0f, 0.0f};

    bool has_bone() const { return !bone.empty(); }
    bool has_offset() const {
        return offset[0] != 0.0f || offset[1] != 0.0f || offset[2] != 0.0f;
    }
};

struct VisusChildRef {
    std::string   visus;           // 子 Visus 名 (同カタログ) または visus ファイル相対パス
    VisusAttach   attach;
    VisusMetadata metadata;

    /// パス参照か (".visus.json" で終わる、 または '/' を含む)。
    bool is_path_ref() const;
};

// ============================================================
// VisusDesc — 描画定義そのもの
// ============================================================

struct VisusDesc {
    std::string                name;     // 唯一の identity
    VisusKind                  kind = VisusKind::NONE;
    std::string                asset;    // kind に応じた主アセット (visus ファイル起点の相対 or 絶対)
    std::vector<VisusPart>     parts;    // kind=model
    std::vector<VisusChildRef> children;
    VisusMetadata              metadata;
    std::vector<VisusPackageRef> packages;  // 全パーツへ重ね掛けするアサイン (§2.5.2)

    /// `part` 名に一致する VisusPart。 無ければ "*" エントリ、 それも無ければ nullptr。
    const VisusPart* find_part(std::string_view part) const;
};

// 規約 key (§2.2)。 ホスト側の読み合わせ用。 Pictor は強制しない。
namespace visus_keys {
inline constexpr const char* kAnimationDefault   = "animation.default";
inline constexpr const char* kAnimationKind      = "animation.kind";
inline constexpr const char* kAnimationLoop      = "animation.loop";
inline constexpr const char* kAnimationSpeed     = "animation.speed";
inline constexpr const char* kAnimationClips     = "animation.clips";
inline constexpr const char* kRenderFlags        = "render.flags";
inline constexpr const char* kRenderLayer        = "render.layer";
inline constexpr const char* kRenderPool         = "render.pool";
inline constexpr const char* kRenderLod          = "render.lod";
inline constexpr const char* kShader             = "shader";
inline constexpr const char* kShaderKeyOverride  = "shader.key_override";
inline constexpr const char* kShaderVertexLayout = "shader.vertex_layout";
inline constexpr const char* kMaterial           = "material";
inline constexpr const char* kMaterialPrefix     = "material.";
inline constexpr const char* kTexturePrefix      = "texture.";
inline constexpr const char* kRiveArtboard       = "rive.artboard";
inline constexpr const char* kTextDefault        = "text.default";
inline constexpr const char* kScaleTargetHeight  = "scale.target_height";
inline constexpr const char* kShaderPackages     = "shader_packages";
} // namespace visus_keys

} // namespace pictor
