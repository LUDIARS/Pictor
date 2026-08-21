# Visus v2 — メタデータ駆動の描画定義 (fbx + パーツ別シェーダ / 入れ子)

起草: 2026-08-20 (neco 方針)。対象: Pictor `include/pictor/visus/*`、KuzuSurvivors `data/visus/` + `SkinnedLayer`、kzs-web `/visus/` エディタ。
関連: `rendering-extensibility-design.md` §2 (方針1 CUSTOM シェーダ)、KS `spec/visus_preview.md`。

## 0. neco 方針 (2026-08-19) と本書の解釈

| neco の言葉 | 解釈 |
|---|---|
| 今は不要なパラメータが多い | v1 `VisusDesc` の typed フィールド群 (ResourceRef の remote/sha256/size/fetch_policy/headers、materials/textures スロット、flags/layer/pool_hint/lod、animation_default、shader_key_override、解決済み handle) は**構造体から外す** |
| Visus は全てのオブジェクトの同一性が取れないのでパラメータは全てメタデータとする | Visus は「ファイル上の定義」であり、Pictor 内の handle (Mesh/Model/Shader/Texture) や Ergo/KS 側のオブジェクトと**同一性を保証できない**。よって Visus が持てる唯一の identity は `name`。それ以外の値は `metadata` (文字列 key → JSON 値) として **解釈をホストに委ねる**。Pictor は構造を検証せず、key 規約だけ文書化する |
| モデル表示の場合 fbx とその中にある各パーツのシェーダを定義する | `kind: model` は `fbx` (パス) + `parts[]` を持つ。part は fbx 内部のパーツ名 (KS `ModelDrawPart::name` = マテリアル diffuse basename / Pictor `ModelDescriptor::material_slots`) で指定し、part ごとに `shader` を定義する |
| Visus は入れ子に出来る。Facial の Visus を Kuzuha が持てる | `children[]` で別 Visus を**名前参照**で保持し、アタッチ先 (bone 等) と metadata を添える。子は独立した Visus ファイルで、親から handle ではなく name で引く |

## 1. v1 の問題 (現状)

- `VisusDesc` (v1) は 20 超のフィールド。KS `data/visus/*.visus.json` 9 本のうち実際に読まれているのは `geometry.kind` / `asset.local_path` / `shader_stages` / `shader_key_override` のみ。materials/textures/flags/animation_default は KS 側で無視され、モデル登録は `SkinnedLayer` のハードコード (`register_model("Player", ...fbx...)`) で行われている。**Visus は描画を駆動していない**。
- JSON に `"mesh": "handle:5"` 等の**解決済み handle を焼いている**。handle はプロセス内連番で、別プロセス・別起動では別物を指す。同一性を表現できないものを永続化している。
- `ResourceRef` の remote 取得設定 (url/sha256/size/policy/headers) は実装 (`FileSystemResourceLoader`) が local しか見ず、使われていない。
- パーツ単位のシェーダ指定が無い (CUSTOM kind は Visus 全体を 1 シェーダに置換するだけ)。
- 入れ子 (Facial を Kuzuha に付ける) を表現できず、`instantiate_visus` も「親子・コンポジションは作らない」と明記している。

## 2. v2 データモデル

### 2.1 JSON スキーマ (version 2)

```jsonc
{
  "version": 2,
  "name": "kuzuha",                       // 唯一の identity。ファイル名 (<name>.visus.json) と一致させる
  "kind": "model",                        // model | rive | primitive | custom | ui | particle | text | group
  "asset": "../../KzSUnity/Assets/3D/Characters/ch_Kuzuha_00/ch_Kuzuha_0000.fbx",
                                          // kind に応じた主アセット (visus ファイル起点の相対 or 絶対)。group は空
  "parts": [                              // kind=model のみ。fbx 内パーツ → シェーダ
    { "part": "T_Cloak_bsc",              // ModelDrawPart::name / material_slot 名。"*" = 既定 (未列挙パーツ)
      "shader": "builtin:pbr",            // 下記 §2.3 のシェーダ参照
      "metadata": { "texture.diffuse": "../../KzSUnity/Assets/3D/Characters/ch_Kuzuha_00/Materials/TEX/T_Cloak_bsc.png" } },
    { "part": "T_Face_bsc",
      "shader": { "vert": "../../shaders/face.vert.spv", "frag": "../../shaders/face.frag.spv" },
      "metadata": { "cull": "none" } }
  ],
  "children": [                           // 入れ子 Visus (名前参照)
    { "visus": "kuzuha_facial",           // 子 Visus 名 (同カタログ内)。パスも可 ("./facial/kuzuha_facial.visus.json")
      "attach": { "bone": "Head", "offset": [0, 0.02, 0.05] },   // 任意。無ければ親 transform そのまま
      "metadata": { "layer": "overlay" } }
  ],
  "metadata": {                           // Visus 自身の付帯情報。Pictor は解釈しない (§2.2 の規約 key のみ文書化)
    "animation.default": "Idle",
    "animation.loop": true,
    "animation.clips": ["Animations/Kuzuha_performance_loop_ver1.fbx", "Animations/Kuzuha_run_ver1.fbx"],
    "render.flags": 2,
    "render.layer": 0,
    "scale.target_height": 1.6
  }
}
```

- **typed フィールドは `name` / `kind` / `asset` / `parts` / `children` / `metadata` の 6 つだけ**。
- `metadata` の値は JSON スカラー / 配列 / オブジェクト。C++ 側は `VisusMetadata` (順序保持の key → `VisusValue` variant) として保持し、`get_string / get_number / get_bool / get_array` のアクセサを提供する。未知 key は保持して round-trip する。
- `kind=custom` のシェーダ参照は `metadata["shader"]` に置く。この値は §2.3 の 3 形式のいずれかとし、`metadata["shader.vertex_layout"]` と同様にランタイムが解釈する。これにより `VisusDesc` の typed フィールドは 6 個のままにする。
- **handle は一切書かない**。resolved handle は実行時の side-table (`VisusRuntime`、§3.2) のみ。
- **ResourceRef は廃止**し、`asset` / shader stage / `texture.*` はすべて**パス文字列**。remote 取得が要るホストは `IResourceLoader` 実装側で URL 変換する (Pictor は持たない)。
- `kind=group` を追加: asset 無し、children だけを束ねる Visus (例: `kuzuha_full` = kuzuha + 武器 + エフェクト)。

### 2.2 規約 key (ホスト側の読み合わせ用、Pictor は強制しない)

| key | 型 | v1 からの移行元 | 用途 |
|---|---|---|---|
| `animation.default` / `animation.loop` / `animation.speed` | string / bool / number | `animation_default.{name,loop,speed}` | 起動時に流すクリップ |
| `animation.kind` | string | `animation_default.kind` | `clip` / `state_machine` / `rive_animation` / `rive_state_machine` |
| `animation.clips` | string[] | (KS ハードコード `player_anims`) | 追加アニメ fbx (visus 起点パス) |
| `render.flags` / `render.layer` / `render.pool` / `render.lod` | number / number / string / number | `flags.*` | `ObjectFlags` / layer / pool hint / 初期 LOD |
| `shader.key_override` | number | `shader_key_override` | CUSTOM kind の shaderKey 下位ビット |
| `shader` | string / object | `shader_stages` | CUSTOM kind のシェーダ参照 (§2.3 の 3 形式) |
| `shader.vertex_layout` | object | `shader_stages.vertex_layout` | CUSTOM kind の頂点入力 (§6.2 形式そのまま) |
| `material.<slot>` | string | 非 model の `materials[].{slot,resource}` | v1 読込互換用。空 slot は `material` |
| `texture.<slot>` | string | `textures[].{slot,resource}` | part または visus 直下のテクスチャ (slot = uniform 名) |
| `rive.artboard` / `text.default` | string | `rive_artboard` / `text_default` | kind 固有 |
| `scale.target_height` | number | (KS `register_model` 第 5 引数) | モデル正規化高さ |

### 2.3 シェーダ参照 (`parts[].shader` / kind=custom の `metadata["shader"]`)

3 形式を許す。いずれも**名前かパス**で、handle は使わない。

1. `"builtin:<name>"` — ホスト組込み pipeline (`builtin:pbr` が既定)。
2. `{ "vert": <path>, "frag": <path>, "comp"?: <path> }` — SPIR-V 直指定 (v1 `shader_stages` と等価。`vertex_layout` は `metadata["shader.vertex_layout"]`)。
3. `"visus:<name>"` — kind=custom の別 Visus をシェーダ定義として参照 (シェーダ定義の共有)。

part が列挙されていない fbx パーツは `"part": "*"` のエントリ、無ければ `builtin:pbr`。

### 2.4 入れ子の意味論

- `children[].visus` は **同じ VisusCatalog 内の name** (or visus ファイル相対パス)。循環参照は load 時に検出してエラー (`visus cycle: a -> b -> a`)。
- `attach.bone` は親が kind=model のときだけ有効。子 instance の transform = 親 transform × bone world × `attach.offset`。bone が無い/親が model でない場合は親 transform そのまま (警告ログ)。
- 子は**独立した Visus** として instantiate され、親の ObjectId 群とは別に返る (`VisusInstance::children`)。親を消すとき子も消す責務はホスト (KS) 側。
- 深さ制限 8 (DoS 対策、`unit_parser_dos_test` と同じ方針)。

## 3. C++ API (Pictor)

### 3.1 型 (`include/pictor/visus/visus.h`)

```cpp
class VisusMetadata;
class VisusValue {
 public:
  using Array = std::vector<VisusValue>;
  using Object = std::shared_ptr<VisusMetadata>;
  using Storage = std::variant<std::monostate, bool, double, std::string, Array, Object>;
  // value/accessor API omitted
};
class VisusMetadata {            // 順序保持 key → VisusValue
  const VisusValue* find(std::string_view key) const;
  std::optional<std::string> get_string(std::string_view) const; // get_number/get_bool/get_array 同様
  void set(std::string key, VisusValue v);
  // begin()/end() で挿入順イテレート
};
struct VisusShaderRef {          // §2.3
  enum class Kind : uint8_t { BUILTIN, STAGES, VISUS };
  Kind kind = Kind::BUILTIN; std::string name = "pbr";   // BUILTIN / VISUS
  std::string vert, frag, comp;                            // STAGES
};
struct VisusPart { std::string part; VisusShaderRef shader; VisusMetadata metadata; };
struct VisusAttach { std::string bone; float offset[3] = {0,0,0}; bool has_bone() const; };
struct VisusChildRef { std::string visus; VisusAttach attach; VisusMetadata metadata; };
enum class VisusKind : uint8_t { NONE, MODEL, RIVE, PRIMITIVE, CUSTOM, UI, PARTICLE, TEXT, GROUP };
struct VisusDesc {
  std::string name; VisusKind kind = VisusKind::NONE; std::string asset;
  std::vector<VisusPart> parts; std::vector<VisusChildRef> children; VisusMetadata metadata;
};
```

削除: `ResourceRef` / `VisusMaterialSlot` / `VisusTextureSlot` / `VisusShaderStages` / `VisusAnimationDefault` / `VisusGeometryKind` / 解決済み handle フィールド / `shader_key_override` 等。`resource_loader.h` の `IResourceLoader::fetch(const ResourceRef&)` は `fetch(std::string_view path)` に変更。

### 3.2 カタログとランタイム

- `VisusCatalog` (新、`visus_catalog.h`): `load_directory(dir)` で `*.visus.json` を name → VisusDesc に読み、`resolve_child(parent, ref)`（name または親 visus ファイル起点の相対パス）/ `resolve_path(desc, rel)` / 循環検出を提供する。**`VisusRegistry` (handle 連番) は廃止** — 同一性は name。
- `VisusRuntime` (新、`visus_runtime.h`): name → 解決済み資源 (`ModelHandle` / `ShaderHandle` / part 名 → shaderKey / 子 instance) の side-table。JSON には出ない。ホストが `resolve(catalog, name, resolver)` で埋める。
- `instantiate_visus(scene, catalog, runtime, name, transform, bounds)` → `VisusInstance { std::vector<ObjectId> objects; std::vector<VisusInstance> children; }`。kind=model は **part ごとに 1 ObjectDescriptor** (v1 の「material slot ごと」を part に置換)、`shaderKey` は part の `VisusShaderRef` から (`builtin:pbr` = 0、STAGES/VISUS = `ShaderKey::with_custom_shader`)。children は §2.4 の transform で再帰。

### 3.3 シリアライザ

- `to_visus_json` は **v2 のみ**出力。
- `from_visus_json` は `version: 1` を受けたら §2.2 の表で v2 に**変換して読む** (model の materials → `parts`、非 model の materials → `material.*` metadata、textures → `texture.*` metadata、handle 文字列は捨てる、`shader_stages` → kind=custom の `metadata["shader"]` STAGES)。変換時は `error` ではなく `warnings` (新 out パラメータ) に `"v1 converted"` を積む。
- `tools/visus_migrate` (小 CLI): ディレクトリ内の v1 を v2 へ書き戻す。KS `data/visus/` 9 本の移行に使う。
- 手書きパーサ方針は維持 (外部依存なし)。`unit_parser_dos_test` に深さ/サイズ上限を v2 の `metadata` 再帰にも適用する。

## 4. ホスト配線 (KuzuSurvivors) — 別タスク

- `SkinnedLayer` のハードコード `register_model(...)` を **VisusCatalog 駆動**へ: `data/visus/*.visus.json` の kind=model を走査し、`asset` + `metadata["animation.clips"]` + part の `texture.diffuse` + `scale.target_height` で `ModelLibrary::register_model` 相当を組む。`enemy_variation.json` の `visus` キーと `player_base.json` の visus 名で引く。
- `ModelDrawPart::name` と `parts[].part` を突き合わせ、part ごとに `SkinnedDraw::shader_key` を決める (現状は visus 全体 1 本)。`SkinnedRenderer::record()` は既に `ShaderKey::is_custom()` で pipeline を切り替えられる。
- `children`: `kuzuha.visus.json` に `kuzuha_facial` (kind=rive or model、顔パーツ) を `attach.bone = "Head"` で持たせ、SkinnedLayer がアクターごとに子を instantiate して bone 追従させる。これが「Facial の Visus を Kuzuha が持てる」の実証。
- kzs-web `/visus/` エディタ: 生 JSON 編集から、`metadata` の key/value 表 + `parts` 表 (fbx からパーツ名を列挙) + `children` ツリーの 3 ペインへ。`kuzu_visus_preview` は v2 ローダで起動。

## 5. 互換と移行順

1. Pictor: 型 + シリアライザ (v1 読込互換) + カタログ + テスト (task 1)。
2. Pictor: instantiate v2 (parts / children) + runtime + migrate CLI (task 2)。
3. KS: `data/visus/` を v2 へ migrate、SkinnedLayer を Visus 駆動化、Facial 子 Visus を 1 本追加、kzs-web エディタ v2 (KS リポの task)。
4. v1 読込互換は KS 移行完了後の次リリースで削除。

## 6. 非ゴール

- マテリアル GUI / ShaderGraph (方針1 phase 2 のまま)。
- remote 資源取得 (Pictor は持たない。`IResourceLoader` 注入で足りる)。
- Visus 間の handle 共有や参照カウント (同一性は name、実体の寿命はホスト)。
