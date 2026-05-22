#pragma once

#include "pictor/core/types.h"
#include "pictor/data/model_data_types.h"  // ModelHandle

#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace pictor {

// ============================================================
// Visus — render-object definition
// ============================================================
// Material 「より大きい」 描画定義。
//
//   Texture / Mesh / Shader / Model     ← raw resource
//           ↓
//   MaterialDesc (texture + scalar + per-pass variants)
//           ↓
//   VisusDesc (geometry + material slot + animation default + render hint)
//           ↓
//   ObjectDescriptor (instance: VisusDesc を transform + bounds で具現化)
//
// VisusDesc 自身は instance 情報 (transform / bounds) を持たない。
// 同じ VisusDesc を 1:N でインスタンス化できるよう、 register 時に
// 外から transform / bounds を与える設計 (`instantiate_visus`)。
//
// Ergo 側プラグインは各プロジェクトの `assets/visus/*.visus.json` を
// scan し、 material / texture registry と組み合わせて GUI 編集する。

using VisusHandle = uint32_t;
constexpr VisusHandle INVALID_VISUS = std::numeric_limits<uint32_t>::max();

// ============================================================
// VisusGeometryKind
// ============================================================
// kind に応じて VisusDesc 内のどのフィールドが使われるかが決まる。

enum class VisusGeometryKind : uint8_t {
    NONE      = 0,
    PRIMITIVE = 1,  // 単一 MeshHandle (旧 MESH)
    MODEL     = 2,  // ModelHandle (skin mesh + rig + clips)
    RIVE      = 3,  // .riv (ベクター)
    UI        = 4,  // 2D / screen overlay
    PARTICLE  = 5,  // particle system
    TEXT      = 6,  // text overlay (string + font + style)
    CUSTOM    = 7   // shader override / decal / その他
};

// ============================================================
// ResourceRef — local 参照 + 任意のネットワーク取得設定
// ============================================================
// 実際の HTTP は Pictor が持たない (`IResourceLoader` を呼び出し側が
// 注入)。 個人データ非保管ルールに従い、 `headers` には credential を
// 直書きせず `"${env:CDN_TOKEN}"` のような env template のみを置く。

struct ResourceRef {
    // ---- Local source ----
    std::string local_path;            // visus file 起点の相対 or 絶対

    // ---- Remote source (任意) ----
    std::string remote_url;            // 空 = ローカルのみ
    std::string sha256;                // 完整性検証 (空ならスキップ)
    uint64_t    size_bytes  = 0;       // 進捗ヒント (0 = 不明)

    enum class FetchPolicy : uint8_t {
        LOCAL_ONLY   = 0,
        CACHE_FIRST  = 1,              // ローカル無ければ DL
        REVALIDATE   = 2,              // sha256 / mtime 不一致なら再 DL
        ALWAYS_FETCH = 3
    };
    FetchPolicy fetch_policy = FetchPolicy::CACHE_FIRST;

    // env template のみ ("${env:NAME}")。 リテラル credential は禁止
    // ([[project_personal_data_rule]])。
    std::vector<std::pair<std::string, std::string>> headers;

    bool empty() const { return local_path.empty() && remote_url.empty(); }
};

// ============================================================
// VisusMaterialSlot — material slot 割当
// ============================================================
// `slot_name` は ModelDescriptor::material_slots に対応。 PRIMITIVE の
// ような単一 mesh ケースでは空文字でよい。 `material_resource` を埋め
// ておけば起動時に material JSON を pull → MaterialRegistry に登録。

struct VisusMaterialSlot {
    std::string    slot_name;
    MaterialHandle material = INVALID_MATERIAL;
    ResourceRef    material_resource;
};

// ============================================================
// VisusTextureSlot — material を経由しない直アサイン
// ============================================================
// CUSTOM shader 等で個別の sampler に直接バインドしたい場合に使う。

struct VisusTextureSlot {
    std::string   slot_name;            // shader uniform 名
    TextureHandle texture = INVALID_TEXTURE;
    ResourceRef   texture_resource;
};

// ============================================================
// VisusAnimationDefault — 起動直後に走らせる default アニメ
// ============================================================

struct VisusAnimationDefault {
    enum class Kind : uint8_t {
        NONE                = 0,
        CLIP                = 1,
        STATE_MACHINE       = 2,
        RIVE_ANIMATION      = 3,
        RIVE_STATE_MACHINE  = 4
    };
    Kind        kind  = Kind::NONE;
    std::string name;             // clip / SM 名 (handle 解決前)
    bool        loop  = true;
    float       speed = 1.0f;
};

// ============================================================
// VisusDesc — 描画定義そのもの
// ============================================================

struct VisusDesc {
    // ---- Identity ----
    std::string name;
    VisusGeometryKind geometry_kind = VisusGeometryKind::NONE;

    // ---- Main asset (kind によって意味が変わる) ----
    //   PRIMITIVE : mesh file       MODEL : model file       RIVE : .riv
    //   UI        : overlay def     PARTICLE : system def    TEXT : font
    //   CUSTOM    : shader file
    ResourceRef asset;

    // ---- Kind-specific extras ----
    std::string rive_artboard;          // RIVE only (空 = default)
    std::string text_default;           // TEXT only (default 表示文字列)

    // ---- Resolved handles (loader が埋める / 直アサインも可) ----
    MeshHandle    mesh           = INVALID_MESH;     // PRIMITIVE
    ModelHandle   model          = INVALID_MODEL;    // MODEL
    ShaderHandle  shader         = INVALID_MESH;     // CUSTOM
    uint32_t      generic_handle = 0;                // RIVE/UI/PARTICLE/TEXT

    // ---- Materials (1 slot = 1 ObjectDescriptor) ----
    std::vector<VisusMaterialSlot> materials;

    // ---- Direct texture override (material 非経由) ----
    std::vector<VisusTextureSlot> textures;

    // ---- Pass / render hint ----
    uint16_t  default_flags = ObjectFlags::DYNAMIC;   // STATIC/TRANSPARENT/...
    uint16_t  layer         = 0;                       // 2-bit, ObjectFlags::set_layer 用
    PoolType  pool_hint     = PoolType::DYNAMIC;
    uint8_t   initial_lod   = 0;

    // ---- Animation default ----
    VisusAnimationDefault animation_default;

    // ---- Shader key override (CUSTOM kind / decal 用) ----
    uint64_t shader_key_override = 0;
};

} // namespace pictor
