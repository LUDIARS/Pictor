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
- **ResourceRef は廃止**し、`asset` / shader stage / `texture.*` はすべて**パス文字列**。remote 取得が要るホストは `IResourceLoader` 実装側で URL 変換する (Pictor は持たない)。同梱の `FileSystemResourceLoader` は configured root の containment を OS の handle / directory fd で検証して symlink 差し替え競合も拒否し、割当て前の configurable size limit (既定 512 MiB) を適用する。
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

### 2.5 シェーダーパッケージ (neco 方針 2026-08-20)

> 「メタデータは元来ある**マテリアル**の上位層にあたる。シェーダーとゲーム中動的に変えるパラメータのペアを**シェーダーパッケージ**として Visus にアサインする。複数アサインできる」

`parts[].shader` (§2.3) は「シェーダ 1 本」しか表せず、そのシェーダが読むパラメータは
`metadata` に平置きされていて対応関係が無い。シェーダーパッケージはこの 2 つを 1 単位に束ねる。

**シェーダーパッケージ = シェーダ参照 + 動的パラメータ (`params`) + パイプライン設定 (`metadata`)**。
再利用資源として独立ファイルに置き、Visus からは **name で参照**する。マテリアル (material JSON)
が「テクスチャ + 定数の束」なのに対し、シェーダーパッケージは「それを描くシェーダごと束ねた上位層」。

#### 2.5.1 ファイル (`<name>.shaderpkg.json`)

```jsonc
{
  "version": 1,
  "name": "toon",                       // 唯一の identity。ファイル名と一致させる
  "shader": "builtin:toon",             // §2.3 の 3 形式 (builtin: / {vert,frag,comp} / visus:)
  "params": {                           // ★ゲーム中に動的に変える値。ここに書くのは既定値
    "rim_power": 2.0,
    "tint": [1.0, 1.0, 1.0, 1.0],
    "texture.ramp": "../tex/toon_ramp.png"
  },
  "metadata": {                         // ★静的なパイプライン設定 (差し替えは再ビルド)
    "shader.vertex_layout": { },
    "shader.key_override": 3,
    "blend": "alpha"
  }
}
```

- `params` と `metadata` の線引き = **ゲーム中に変わるか**。`params` は変わる (ホットリロード対象外、
  プログラム経路で書き換える。#736 の方針と同じ)。`metadata` は pipeline の作り方を決めるので変えない。
- `params` の key は uniform 名 / `texture.<slot>` / `material.<slot>`。Pictor は**解釈しない** (§2.2 と同じく
  ホストへ委ねる)。値は JSON スカラー / 配列 / オブジェクト。
- shader stage のパスは**パッケージファイル起点**で解決する (Visus ファイル起点ではない)。

#### 2.5.2 Visus へのアサイン

```jsonc
{
  "version": 2, "name": "kuzuha", "kind": "model",
  "shader_packages": [                    // ★Visus 直下 = 全パーツへ重ね掛け。配列順 = 描画順
    "toon",
    { "package": "outline", "params": { "width": 0.02 } }
  ],
  "parts": [
    { "part": "T_Face_bsc",
      "shader": "builtin:pbr",            // base パス (従来通り。省略時 builtin:pbr)
      "shader_packages": [                // ★パーツ別。Visus 直下の列の後ろに append
        { "package": "outline", "enabled": false },   // このパーツだけ輪郭を外す
        { "package": "skin_sss", "params": { "thickness": 0.4 } }
      ] }
  ]
}
```

- 参照は文字列 (`"toon"`) か object (`{ "package": name, "enabled"?: bool, "params"?: {}, "metadata"?: {} }`)。
  **インライン定義は許さない** — パッケージは常に名前を持つ再利用資源とする。
- `params` / `metadata` はパッケージ既定値への**上書きマージ** (key 単位。未指定 key は既定のまま)。

#### 2.5.3 実効パッケージ列 (重ね掛け + パーツ別のマージ規則)

1. Visus 直下 `shader_packages` を配列順に並べる。
2. パーツの `shader_packages` を順に見る。
   - 同じ package 名が既に列にある → **その位置のまま** `enabled` / `params` / `metadata` を上書きマージ。
   - 無い → 列の末尾へ追加。
3. `enabled: false` のエントリを除外したものが**実効列**。
4. 描画パスは **base (typed `shader`) が 1 本 + 実効列を順に重ね掛け**。
   実効列が空なら従来と同じ 1 draw で、v2 既存ファイルの互換は保たれる。

実装は package 名と metadata key の索引を一時的に作り、上記の順序を保ったまま入力件数に
対して線形にマージする。大きな外部 JSON で同名探索が二乗時間になることは許容しない。

重ね掛けの順序 = `SceneRegistry` への登録順。深度書き込み / ブレンドは各パッケージの `metadata` を
見てホストの pipeline が決める (Pictor は順序だけ保証する)。

#### 2.5.4 C++ API

```cpp
struct VisusShaderPackage {            // ファイル 1 本 = 1 パッケージ
    std::string    name;
    VisusShaderRef shader;
    VisusMetadata  params;             // 動的パラメータの既定値
    VisusMetadata  metadata;           // pipeline 設定
};
struct VisusPackageRef {               // Visus / part からの参照
    std::string   package;
    bool          enabled = true;
    VisusMetadata params;              // 既定値への上書き
    VisusMetadata metadata;
};
/// Visus 直下 + part の参照列を §2.5.3 の規則でマージした実効列。
std::vector<VisusPackageRef> visus_effective_packages(
    const std::vector<VisusPackageRef>& visus_level,
    const std::vector<VisusPackageRef>& part_level);

class VisusPackageCatalog {            // *.shaderpkg.json を name → package
    size_t load_directory(const std::string& dir, ...);
    const VisusShaderPackage* find(std::string_view name) const;
    std::string resolve_path(std::string_view package, std::string_view rel) const;
};
```

`VisusRuntime::resolve` は `const VisusPackageCatalog*` を任意引数で受け (既定 nullptr = 従来通り)、
実効列を解決して `VisusResolvedPart::packages` / `VisusResolved::packages` に
`VisusResolvedPackage { package, shader, shader_key, params }` として積む。`params` は
**既定 + 上書きをマージ済みの実値**で、これが instantiate 後の動的変更の初期値になる。

`instantiate_visus` は part ごとに base + 実効パッケージ数の `ObjectDescriptor` を登録し、
`VisusInstance::bindings` に `{ part, package, object, params }` を返す。ホストは

```cpp
inst.set_param("T_Face_bsc", "toon", "rim_power", 3.5);   // params_revision が上がる
```

で書き換え、`params_revision()` の変化を見て GPU へ再アップロードする。Pictor はパラメータの
**正本と改訂番号を持つだけ**で、uniform への実バインドはホスト (§4) の責務。


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
- `VisusRuntime` (新、`visus_runtime.h`): name → 解決済み資源 (`ModelHandle` / `ShaderHandle` / 実 draw part 名 → `MeshHandle` + shaderKey / 子 instance) の side-table。JSON には出ない。ホストが `resolve(catalog, name, resolver)` で埋める。`IVisusResolver::model_parts(ModelHandle)` は fbx の実 draw part 名と描画用 mesh を列挙し、Visus の `parts[]` は exact / `"*"` のシェーダ設定としてその一覧へ適用する。
- `VisusRuntime::invalidate(name)` は、解決済みの `name` があれば side-table 全体を破棄する。children と `visus:` shader は複数 root から共有され、runtime は逆依存グラフを保持しないため、部分無効化で古い handle を再利用しないことを優先する。
- `instantiate_visus(scene, catalog, runtime, name, transform, bounds)` → `VisusInstance { std::vector<ObjectId> objects; std::vector<VisusInstance> children; uint32_t generic_handle; }`。kind=model は resolver が列挙した**実 draw part ごとに 1 ObjectDescriptor**を作り、`mesh` と `shaderKey` を設定する (`builtin:pbr` = 0、STAGES/VISUS = `ShaderKey::with_custom_shader`)。primitive は解決済み mesh がある場合だけ登録する。rive/ui/particle/text は無効 mesh の object を登録せず `generic_handle` をホストへ返し、custom は model part から参照する shader 定義として扱う。children は §2.4 の transform で再帰。

### 3.3 シリアライザ

- `to_visus_json` は **v2 のみ**出力。
- `from_visus_json` は `version: 1` を受けたら §2.2 の表で v2 に**変換して読む** (model の materials → `parts`、非 model の materials → `material.*` metadata、textures → `texture.*` metadata、handle 文字列は捨てる、`shader_stages` → kind=custom の `metadata["shader"]` STAGES)。変換時は `error` ではなく `warnings` (新 out パラメータ) に `"v1 converted"` を積む。
- `tools/visus_migrate` (小 CLI): ディレクトリ内の v1 を v2 へ書き戻す。KS `data/visus/` 9 本の移行に使う。
- 手書きパーサ方針は維持 (外部依存なし)。`unit_parser_dos_test` に深さ/サイズ上限を v2 の `metadata` 再帰にも適用する。

## 4. ホスト配線 (KuzuSurvivors) — 別タスク

- `SkinnedLayer` のハードコード `register_model(...)` を **VisusCatalog 駆動**へ: `data/visus/*.visus.json` の kind=model を走査し、`asset` + `metadata["animation.clips"]` + part の `texture.diffuse` + `scale.target_height` で `ModelLibrary::register_model` 相当を組む。`enemy_variation.json` の `visus` キーと `player_base.json` の visus 名で引く。
- `ModelDrawPart::name` と `parts[].part` を突き合わせ、part ごとに `SkinnedDraw::shader_key` を決める (現状は visus 全体 1 本)。`SkinnedRenderer::record()` は既に `ShaderKey::is_custom()` で pipeline を切り替えられる。
- `children`: `kuzuha.visus.json` に `kuzuha_facial` (kind=rive or model、顔パーツ) を `attach.bone = "Head"` で持たせ、SkinnedLayer がアクターごとに子を instantiate して bone 追従させる。これが「Facial の Visus を Kuzuha が持てる」の実証。
- **シェーダーパッケージ (§2.5)**: `data/shaderpkg/*.shaderpkg.json` を `VisusPackageCatalog` で読み、
  `VisusRuntime::resolve(..., &packages)` に渡す。`SkinnedRenderer` は `VisusInstance::bindings` の
  ObjectId を重ね掛けパスとして描き、`params` を uniform へ流す (`params_revision` が変わったときだけ
  再アップロード)。ゲーム中の値変更 (被弾フラッシュ・輪郭の太さ等) はこの経路。
- kzs-web `/visus/` エディタ: 生 JSON 編集から、`metadata` の key/value 表 + `parts` 表 (fbx からパーツ名を列挙) + `children` ツリーの 3 ペインへ。`kuzu_visus_preview` は v2 ローダで起動。

## 5. 互換と移行順

### 5.1 C++ source compatibility

本変更は `VisusDesc`、`VisusRegistry`、`IResourceLoader::fetch` を置換する**意図的な
source-breaking migration** であり、旧 handle-based C++ API の adapter は提供しない。
JSON は v1 読込互換を維持するが、C++ consumer は下記順序の host wiring が完了するまで
pre-v2 の Pictor revision を pin し、Pictor 更新と consumer の API 移行を同一リリースで行う。
Pictor C API はこれらの Visus C++ 型を公開していないため `PICTOR_C_API_VERSION` は変更しない。

1. Pictor: 型 + シリアライザ (v1 読込互換) + カタログ + テスト (task 1)。
2. Pictor: instantiate v2 (parts / children) + runtime + migrate CLI (task 2)。
3. Pictor: シェーダーパッケージ (§2.5) の型 / カタログ / runtime / instantiate + テスト (task 3)。
4. KS: `data/visus/` を v2 へ migrate、SkinnedLayer を Visus 駆動化、Facial 子 Visus を 1 本追加、kzs-web エディタ v2 (KS リポの task)。
5. v1 読込互換は KS 移行完了後の次リリースで削除。

## 6. 非ゴール

- マテリアル GUI / ShaderGraph (方針1 phase 2 のまま)。
- remote 資源取得 (Pictor は持たない。`IResourceLoader` 注入で足りる)。
- Visus 間の handle 共有や参照カウント (同一性は name、実体の寿命はホスト)。
