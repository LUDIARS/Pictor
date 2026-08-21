---
task: visus-v2-ks-wiring-20260820
project: KuzuSurvivors
kind: 実装
created: 2026-08-20T00:00:00.000Z
memory_links:
  - spec/feature/visus-v2-design.md
  - spec/tasks/2026-08-20-visus-v2-instantiate-runtime.md
---
# Visus v2 — KuzuSurvivors 配線 (Visus 駆動モデル登録 / パーツ別シェーダ / Facial 子 Visus / エディタ)

> 実施場所は **KuzuSurvivors リポ** (この task ファイルは Pictor 側の設計に紐づけるための写し。KS リポの `spec/tasks/` に同内容を置いてそちらを正とする)。

## 目的
[visus-v2-design.md](../feature/visus-v2-design.md) §4。`SkinnedLayer` のハードコード
`register_model(...)` を `data/visus/*.visus.json` (v2) 駆動へ置き換え、fbx 内パーツごとの
シェーダ切替と、Kuzuha が Facial Visus を子として持つ構成を実機で成立させる。

## 前提
- Pictor task 1 / task 2 がローカル main に入っている (Pictor submodule / 参照を bump)。

## 完了条件
- [ ] `data/visus/` 9 本を `visus_migrate` で v2 化し、`kuzuha.visus.json` (player) を追加: `asset` = ch_Kuzuha fbx、`metadata["animation.clips"]` に現行 `player_anims` 5 本、`parts` に `T_Cloak_bsc` 等の part + `texture.diffuse`、`scale.target_height`。`player_base.json` に `"visus": "kuzuha"`。
- [ ] `SkinnedLayer`: `VisusCatalog::load_directory("data/visus")` → kind=model を `ModelLibrary::register_model` 相当へ (fbx / clips / diffuse / target_height を metadata から)。ハードコードのパス列挙を削除 (`KzSUnity` ルート解決は `asset_root.h` のまま、visus 起点相対で書く)。
- [ ] part 別シェーダ: `ModelDrawPart::name` ↔ `parts[].part` を突き合わせ、`SkinnedDraw::shader_key` を part 単位に。`register_custom_shaders_` は v2 `VisusShaderRef` (STAGES / `visus:` 参照) から `CustomShaderDef` を組む。`kuzu_custom_demo` は kind=custom のまま動くこと (`KUZU_CUSTOM_SHADER_DEMO=1`)。
- [ ] Facial 子 Visus: `kuzuha_facial.visus.json` (kind=rive または model の顔パーツ) を `kuzuha.visus.json` の `children[]` に `attach.bone = "Head"` で追加。SkinnedLayer がアクターごとに子を instantiate し、毎フレーム Head bone のワールド行列へ追従させる。プレイヤー破棄時に子も破棄。
- [ ] `enemy_variation.json` の `visus` 名経由でも同じ経路 (`Enemy/*` キーの直書きを撤去)。
- [ ] kzs-web `/visus/`: 生 JSON 編集 → `metadata` key/value 表 + `parts` 表 (fbx からパーツ名列挙: `ModelLibrary` の dump か `kuzu_visus_preview --list-parts`) + `children` ツリーの 3 ペイン。保存は v2 のみ。`kuzu_visus_preview` を v2 ローダ + children 描画対応。
- [ ] `spec/rendering_overview.md` / `spec/visus_preview.md` を v2 に更新。
- [ ] ビルド緑。実機確認は no-run 規約により Memoria 確認タスクとして登録 (`kuzu_visus_preview --visus kuzuha.visus.json` で Facial が Head に付いて動く)。

## スコープ (編集可ディレクトリ)
- KS: `data/visus/`, `data/player_base.json`, `data/enemy_variation.json`, `src/render/layers/skinned_layer.*`, `src/render/model_library.*`, `src/render/skinned_renderer.*` (shader_key の part 化のみ), `src/tools/visus_preview_main.cpp`, `tools/kzs-web/src/plugins/visus/`, `spec/`
