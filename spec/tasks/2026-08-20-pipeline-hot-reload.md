---
task: pipeline-hot-reload-20260820
project: Pictor
kind: 実装
created: 2026-08-20T01:00:00.000Z
memory_links:
  - spec/feature/pipeline-hot-reload.md
  - spec/feature/rendering-extensibility-design.md
---
# パイプライン設定 + シェーダのホットリロード (値変更はプログラム対応)

## 目的
[pipeline-hot-reload.md](../feature/pipeline-hot-reload.md)。レンダリングパイプライン可視化
(Ergo Profile Editor) のために、profile JSON と compiled SPIR-V の変更を実行中に反映する。
値の変更はホットリロード対象にしない (register_custom_profile + reload_active_profile /
set_config / push constant でプログラムから流し込む)。

## 完了条件
- [x] `core/file_watch.{h,cpp}`: mtime ポーリングの `FileWatchSet` (settle 待ち既定 200ms、存在↔不在も変更扱い、OS 監視 API / スレッド不使用)。
- [x] `pipeline/pipeline_hot_reload.{h,cpp}`: group 名付き監視管制 `PipelineHotReload` (`watch` / `watch_directory(ext フィルタ)` / `on_group` / `poll` interval 間引き既定 500ms)。コールバックは poll スレッド上で同期実行。
- [x] `PictorRenderer::profile_source_path()` / `reload_profile_from_source(error*)` — active な file-backed profile の読込元と初回読込時の基底を記憶し再適用 (key 削除は前回値を残さず基底へ復帰、apply_profile → 下流再構成 → compiled graph 再 compile は既存経路)。programmatic な選択/同名置換と shutdown では source を解除し、stale watcher が旧 file profile を再有効化しない。
- [x] `ShaderRegistry::rebuild_pipelines()` — 初回 build の引数を記憶し、size/alignment/header magic を検査した SPIR-V から候補 pipeline 群を生成。全件成功後だけ内部 wait idle で差し替え、失敗時は last-known-good pipeline 群を維持。Vulkan 無効時は no-op。
- [x] テスト: `unit_file_watch_test` (settle / 連続書込 / 不在→出現 / 削除)、`unit_pipeline_hot_reload_test` (group 分配 / interval 間引き / symlink・拡張子フィルタ / バッチ発火 / file source の programmatic 選択・同名置換・shutdown 時解除)、`unit_shader_registry_test` (headless rebuild no-op 契約)。
- [x] MSVC ローカルビルド緑。GCC は CI で確認。テスト実行は no-run 規約により Revisor に委ねる。

## スコープ (編集可ディレクトリ)
- `include/pictor/core/`, `src/core/`, `include/pictor/pipeline/`, `src/pipeline/`, `include/pictor/shader/`, `src/shader/`, `tests/`, `CMakeLists.txt`, `tests/CMakeLists.txt`

## 非スコープ (ホスト配線 = 別タスク)
- KS `kuzu_visus_preview` / `GameRenderer` の watch 配線、Ergo render_pipeline エディタ側 (spec §2)。
- GLSL→SPIR-V コンパイル、GI/decal 等の組み込み固定シェーダのリロード。
