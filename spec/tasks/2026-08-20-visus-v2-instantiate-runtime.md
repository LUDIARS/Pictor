---
task: visus-v2-instantiate-runtime-20260820
project: Pictor
kind: 実装
created: 2026-08-20T00:00:00.000Z
memory_links:
  - spec/feature/visus-v2-design.md
  - spec/tasks/2026-08-20-visus-v2-schema-serializer.md
---
# Visus v2 — runtime 解決・instantiate・v1 migrate

## 目的

[visus-v2-design.md](../feature/visus-v2-design.md) §2.3〜§3.3 に従い、名前で管理する
`VisusDesc` を実行時 handle へ解決する side-table、model part と children を
`ObjectDescriptor` へ具現化する経路、および v1 JSON を安全に v2 へ書き戻す移行手段を提供する。
JSON に process-local handle を永続化せず、資源ロードと寿命管理はホストへ委譲する。

## 完了条件

- [x] `IVisusResolver` と `VisusRuntime` が model / primitive / generic asset、part 別 shader・material、`visus:` shader 参照を解決し、結果を name keyed side-table に保持する。
- [x] `instantiate_visus` が有効な model part / primitive だけを `SceneRegistry` へ登録し、generic handle と children tree を `VisusInstance` で返す。
- [x] children は循環と深さ超過を打ち切り、bone 解決成功時だけ `offset × bone × parent`、bone 未指定時は `offset × parent`、bone 解決失敗時は parent transform そのものへ安全に縮退する。
- [x] `visus_migrate_{file,directory}` と CLI が v1 を canonical v2 へ変換し、dry-run、決定的な列挙順、入力サイズ制限、symlink 拒否、同一ディレクトリ staging からの原子的置換を提供する。
- [x] runtime / instantiate / migrate の登録 unit test が part 解決、shader 共有、children transform、循環・欠落、dry-run・冪等性・置換失敗を覆う。
- [ ] MSVC / GCC のビルドと登録 CTest は Revisor CI で確認する。本レビューは no-run 制約のため未実行。

## スコープ

- `include/pictor/visus/visus_runtime.h`, `include/pictor/visus/visus_instantiator.h`, `include/pictor/visus/visus_migrate.h`
- `src/visus/visus_runtime.cpp`, `src/visus/visus_instantiator.cpp`, `src/visus/visus_migrate.cpp`
- `tools/visus_migrate/`, 対応する `tests/unit_visus_*`, `CMakeLists.txt`, `tests/CMakeLists.txt`

## 実装メモ (2026-08-20)
- `IVisusResolver` は `load_mesh / load_model / model_parts / register_shader_stages / builtin_shader / load_generic / load_material / bone_transform` の薄い virtual。`model_parts` は実 draw part 名 + `MeshHandle` を返し、Visus の exact / `"*"` 設定を実 geometry へ結び付ける。既定はすべて未対応 (INVALID / false / 空) で、error 引数がある capability は理由も返す。受け取るパスは `VisusCatalog::resolve_path` 済み。
- `VisusRuntime::resolve` は children も再帰解決 (循環 / 深さ 8 は warnings)。 `visus:<name>` 参照は 1 段だけ辿り、 参照先 (kind=custom) も side-table に入れて共有する。 part の `shader.key_override` は visus のものに優先。
- `instantiate_visus(scene, catalog, runtime, name, transform, bounds, out, warnings, resolver)` → `VisusInstance { name, transform, objects, children, generic_handle }`。model/primitive は有効な実 mesh だけを SceneRegistry へ登録し、rive/ui/particle/text は無効 mesh object を作らず generic handle をホストへ返す。`render.pool` は ObjectDescriptor に受け皿が無いので読まない。`visus_compose(local, parent)` (行優先 = local × parent) を公開。
- `visus_migrate_{file,directory}` はライブラリ側 (常にコンパイル)、 CLI `tools/visus_migrate/main.cpp` は薄いラッパ。`visus_migrate` target は `PICTOR_BUILD_TOOLS` または `PICTOR_BUILD_TESTS` が有効な非 mobile/WebGL build で作り、library-only consumer に余計な executable を持ち込まない。
- Revisor 修正: migrate の実書込みは同一ディレクトリの staging へ完全書込み後に原子的置換し、失敗時は元 v1 を保持する。directory scan / direct-file API の symlink と 16 MiB 超の入力は拒否する。
- Revisor autofix (#725) が足したサロゲート テストは raw string 内 `\uD800` を MSVC が C3850 にするため通常リテラルへ書き換えた。

## 実装メモ (2026-08-21 / Revisor autofix #729)
- version 判定を `visus_document_version` として serializer に切り出し、読込経路と migrate 経路で共有した。`version` を落とした v1 文書 (v1 固有トップレベル key あり) を migrate が `already-v2` と誤報告して黙って移行を飛ばしていた。
- `VisusRuntime::resolve_rec_` に (name, depth) の訪問メモを入れた (`VisusCatalog::validate_node_` と同じ不変条件)。カタログは DAG なので、共有された部分木を親ごとに歩き直すと訪問数が branching^depth になり同じ警告が重複していた。
- kind=primitive は parts[] を持てないため、visus metadata の `material` を `VisusRuntime` が解決し (`VisusResolved::material`)、`instantiate_visus` が primitive の `customShader` / `material` / 解決済み `shaderKey` を ObjectDescriptor へ運ぶようにした。v1 変換は非 model の `materials[]` をこの metadata へ落とすので、以前は移行後に material が失われていた。
- `visus_migrate_file` 単体経路にもディレクトリ走査と同じ symlink 拒否を入れた (rename 置換がリンク自身を潰すため)。`VisusCatalog::load_directory` は status 取得に失敗した候補も理由付きで errors へ出す (黙って落とさない)。
- 追加テスト: version 欠落 v1 の移行と冪等性、共有部分木の重複訪問、primitive material 解決、primitive の shader/material 引き渡し。
