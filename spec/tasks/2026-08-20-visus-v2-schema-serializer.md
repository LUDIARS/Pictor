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
- [ ] `include/pictor/visus/visus.h` を v2 型へ置換 (`VisusValue` / `VisusMetadata` / `VisusShaderRef` / `VisusPart` / `VisusAttach` / `VisusChildRef` / `VisusKind` / `VisusDesc`)。v1 の `ResourceRef` / `VisusMaterialSlot` / `VisusTextureSlot` / `VisusShaderStages` / `VisusAnimationDefault` / `VisusGeometryKind` / 解決済み handle / `shader_key_override` を削除。
- [ ] `VisusMetadata` に `find / get_string / get_number / get_bool / get_array / set` と挿入順イテレート。未知 key は保持して round-trip。
- [ ] `visus_serializer.{h,cpp}`: `to_visus_json` は v2 のみ emit。`from_visus_json` は `version:2` を読み、`version:1` は §2.2 の表で v2 へ変換して読む (handle 文字列は捨てる、`shader_stages` → kind=custom の `metadata["shader"]` STAGES、materials/textures → `texture.*`)。変換時は新 out パラメータ `warnings` に `"v1 converted"`。外部依存なしの手書きパーサ方針維持。
- [ ] `VisusRegistry` (handle 連番) を削除し `visus_catalog.{h,cpp}` を新設: `load_directory(dir)` → name → VisusDesc、`resolve_child(parent, ref)`（name または親 visus ファイル起点の相対パス）、`resolve_path(desc, rel)`、children の循環検出 (`visus cycle: a -> b -> a`) と深さ上限 8。
- [ ] `resource_loader.h`: `IResourceLoader::fetch(const ResourceRef&)` → `fetch(std::string_view path)`。`FileSystemResourceLoader` を追従。
- [ ] v1 型の残存参照を一掃 (`grep -r "ResourceRef\|VisusGeometryKind\|VisusShaderStages" include src tests` が空)。現時点で型として参照しているのは `include/pictor/visus/` / `src/visus/` / `tests/` のみ。`include/pictor/shader/shader_registry.h` と `include/pictor/core/types.h` は doc コメントで v1 名 (`VisusDesc::shader` / `shader_stages`) に言及しているだけなので、コメント文言を v2 (`VisusShaderRef` / `metadata["shader"]`) へ更新する。
- [ ] テスト: `tests/unit_visus_serializer_test.cpp` (v2 round-trip / metadata 未知 key 保持 / v1 → v2 変換が §2.2 表どおり / parts・children の読み書き)、`tests/unit_visus_catalog_test.cpp` (循環検出 / 深さ上限 / 相対パス解決)、`unit_parser_dos_test` に metadata 再帰の深さ・サイズ上限を追加。`unit_visus_instantiator_test` は task 2 で置換するまで v2 型でコンパイルが通る最小修正。
- [ ] GCC (CI) でビルド緑 ([[project-pictor-gcc-ci]]: MSVC で無音の GCC エラーに注意)。ヘッダ変更なので `rm -rf build` からクリーンビルド。

## スコープ (編集可ディレクトリ)
- `include/pictor/visus/`, `src/visus/`, `tests/` (visus 関連), `CMakeLists.txt` / `tests/CMakeLists.txt`
- v1 名に言及している `include/pictor/shader/shader_registry.h` / `include/pictor/core/types.h` の doc コメント差し替えのみ (`pipeline_profile_serializer` は手書きパーサ方針を共有するだけで Visus 型に依存していないため対象外)

## 非スコープ
- instantiate / runtime / migrate CLI (task 2)、KS 側配線 (KS リポ task)
