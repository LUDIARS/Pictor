---
task: visus-v2-instantiate-runtime-20260820
project: Pictor
kind: 実装
created: 2026-08-20T00:00:00.000Z
memory_links:
  - spec/feature/visus-v2-design.md
  - spec/tasks/2026-08-20-visus-v2-schema-serializer.md
---
# Visus v2 — instantiate (parts / children) + VisusRuntime + migrate CLI

## 目的
[visus-v2-design.md](../feature/visus-v2-design.md) §2.4 / §3.2 / §3.3。
Visus を「描画を駆動する定義」にする: kind=model は fbx 内パーツごとに
ObjectDescriptor とシェーダを割り当て、children を bone アタッチ付きで再帰
instantiate する。解決済み handle は JSON ではなく `VisusRuntime` に持つ。

## 前提
- task 1 (`visus-v2-schema-serializer`) マージ済み。

## 完了条件
- [ ] `visus_runtime.{h,cpp}` 新設: name → `{ ModelHandle / MeshHandle / ShaderHandle, part 名 → shaderKey, 子 instance }` の side-table。`resolve(catalog, name, IVisusResolver&)` でホストが埋める (`IVisusResolver` = `load_model(path)` / `register_shader(VisusShaderRef, metadata)` / `load_rive(path)` … の薄いインターフェース。kind=custom のシェーダ参照は `metadata["shader"]` から読む。Pictor 同梱実装は model/shader のみ、他は未解決 = INVALID)。
- [ ] `visus_instantiator.{h,cpp}` を v2 へ: `instantiate_visus(scene, catalog, runtime, name, transform, bounds)` → `VisusInstance { objects; children; }`。kind=model は **part ごとに 1 ObjectDescriptor**、`shaderKey` は `VisusShaderRef` から (`builtin:pbr` = 0 / STAGES・VISUS = `ShaderKey::with_custom_shader(metadata["shader.key_override"], handle)`)。`"*"` part を既定に、未列挙パーツは `builtin:pbr`。
- [ ] children 再帰: 親 kind=model かつ `attach.bone` 指定 → 親 transform × bone world (rig から) × offset。それ以外は親 transform (警告ログ)。深さ 8 超・循環はエラー。kind=group は objects 空で children のみ。
- [ ] `render.flags` / `render.layer` / `render.pool` / `render.lod` metadata を ObjectDescriptor へ写す (無ければ v1 既定値: DYNAMIC / 0 / DYNAMIC / 0)。
- [ ] `tools/visus_migrate` (小 CLI、外部依存なし): `visus_migrate <dir>` で v1 を読み v2 で書き戻す。`--dry-run` で差分表示のみ。
- [ ] テスト: `unit_visus_instantiator_test` を v2 へ書き直し (parts → ObjectDescriptor 数 / part 別 shaderKey / `*` 既定 / children の transform 合成 / group / 循環・深さエラー)、`unit_visus_runtime_test` (resolver 注入で handle が埋まる、未解決は INVALID)、migrate の round-trip テスト。
- [ ] GCC (CI) でビルド緑、クリーンビルド確認。

## スコープ (編集可ディレクトリ)
- `include/pictor/visus/`, `src/visus/`, `tools/visus_migrate/`, `tests/`, `CMakeLists.txt`

## 非スコープ
- KS `SkinnedLayer` の Visus 駆動化・Facial 子 Visus・kzs-web エディタ (KS リポ task、本 spec §4)
