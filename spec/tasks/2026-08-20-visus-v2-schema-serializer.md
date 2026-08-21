---
task: visus-v2-schema-serializer-20260820
project: Pictor
kind: 実装
created: 2026-08-20T00:00:00.000Z
memory_links:
  - spec/feature/visus-v2-design.md
  - spec/feature/rendering-extensibility-design.md
---
# Visus v2 — 型 + メタデータ + シリアライザ (v1 読込互換) + カタログ

## 目的
[visus-v2-design.md](../feature/visus-v2-design.md) §2〜§3.3。`VisusDesc` を
`name / kind / asset / parts / children / metadata` の 6 フィールドに縮約し、
それ以外の値は `VisusMetadata` (順序保持 key → JSON 値) として保持する。
handle・ResourceRef・typed スロットは構造体から消す。同一性は `name` のみ。

## 完了条件
- [x] `include/pictor/visus/visus.h` を v2 型へ置換 (`VisusValue` / `VisusMetadata` / `VisusShaderRef` / `VisusPart` / `VisusAttach` / `VisusChildRef` / `VisusKind` / `VisusDesc`)。v1 の `ResourceRef` / `VisusMaterialSlot` / `VisusTextureSlot` / `VisusShaderStages` / `VisusAnimationDefault` / `VisusGeometryKind` / 解決済み handle / `shader_key_override` を削除。
- [x] `VisusMetadata` に `find / get_string / get_number / get_bool / get_array / set` と挿入順イテレート。未知 key は保持して round-trip。
- [x] `visus_serializer.{h,cpp}`: `to_visus_json` は v2 のみ emit。`from_visus_json` は `version:2` を読み、`version:1` は §2.2 の表で v2 へ変換して読む (handle 文字列は捨てる、`shader_stages` → kind=custom の `metadata["shader"]` STAGES、model materials → `parts`、非 model materials → `material.*`、textures → `texture.*`)。変換時は新 out パラメータ `warnings` に `"v1 converted"`。外部依存なしの手書きパーサ方針維持。
- [x] `VisusRegistry` (handle 連番) を削除し `visus_catalog.{h,cpp}` を新設: `load_directory(dir)` → name → VisusDesc、`resolve_child(parent, ref)`（name または親 visus ファイル起点の相対パス）、`resolve_path(desc, rel)`、children の循環検出 (`visus cycle: a -> b -> a`) と深さ上限 8。
- [x] `resource_loader.h`: `IResourceLoader::fetch(const ResourceRef&)` → `fetch(std::string_view path)`。`FileSystemResourceLoader` を追従。
- [x] `pipeline_profile_serializer` / `shader_registry.h` / `core/types.h` 等に v1 型を使う実行コードが残っていない。旧型名は v1 移行処理と設計履歴を説明するコメントに限る。
- [x] テスト: `tests/unit_visus_serializer_test.cpp` (v2 round-trip / metadata 未知 key 保持 / v1 → v2 変換が §2.2 表どおり / parts・children の読み書き / 未対応 future version 拒否)、`tests/unit_visus_catalog_test.cpp` (循環検出 / 深さ上限 / 相対パス解決 / 共有 subtree の有界検査)、`unit_visus_resource_loader_test` (root 内読込 / traversal・絶対パス拒否)、`unit_parser_dos_test` に metadata 再帰の深さ・サイズ上限を追加。`unit_visus_instantiator_test` は metadata → ObjectDescriptor 共通フィールドと shader key 予約ビット保護だけを検証し、SceneRegistry 登録 API は handle 解決可能な task 2 で導入する。
- [ ] Revisor 修正後の MSVC / GCC ビルドと登録テストは CI で確認する ([[project-pictor-gcc-ci]]: MSVC で無音の GCC エラーに注意)。本レビューは no-run 制約のため未実行。

## スコープ (編集可ディレクトリ)
- `include/pictor/visus/`, `src/visus/`, `tests/` (visus 関連), `CMakeLists.txt` / `tests/CMakeLists.txt`
- v1 名に言及している `include/pictor/shader/shader_registry.h` / `include/pictor/core/types.h` の doc コメント差し替えのみ (`pipeline_profile_serializer` は手書きパーサ方針を共有するだけで Visus 型に依存していないため対象外)

## 非スコープ
- instantiate / runtime / migrate CLI (task 2)、KS 側配線 (KS リポ task)

## 実装メモ (2026-08-20)
- 追加: `visus_metadata.{h,cpp}` (`VisusValue` / `VisusMetadata`)、 `visus_types.cpp` (kind / `VisusShaderRef` ↔ 値)、 `src/visus/visus_json.{h,cpp}` (内部 JSON パーサ・エミッタ、 深さ上限共有)、 `src/visus/visus_v1_compat.{h,cpp}` (v1 → v2 変換)、 `visus_catalog.{h,cpp}`、 `tests/unit_visus_metadata_test.cpp` / `tests/unit_visus_catalog_test.cpp`。
- `visus_instantiator` は `render.*` metadata から ObjectDescriptor 共通フィールドを作る helper のみ。handle 解決前の非描画 object は SceneRegistry に登録せず、instantiate API / part 別シェーダ / children 再帰は task 2。
- テストは実行していない (セッション方針 / Pictor no-run 規約)。 Revisor の登録テストで確認する。
