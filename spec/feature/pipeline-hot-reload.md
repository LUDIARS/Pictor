# Pipeline Hot Reload — パイプライン設定とシェーダのホットリロード

起草: 2026-08-20 (neco 方針)。対象: Pictor `core/file_watch` / `pipeline/pipeline_hot_reload` / `ShaderRegistry` / `PictorRenderer`。
関連: `rendering-extensibility-design.md` (系統A/B)、`pipeline-profile-config.md`、Ergo render_pipeline プラグイン (Profile Editor)。

## 0. neco 方針 (2026-08-20)

> レンダリングパイプラインを可視化するにあたり、ホットリロードが必要。
> パイプライン設定とシェーダーのホットリロードに対応して。
> 値の変更はホットリロードせずプログラムで対応。

| 対象 | 方式 |
|---|---|
| パイプライン設定 (`*.profile.json`) | **ファイル監視でホットリロード** — 変更検出 → `PictorRenderer::reload_profile_from_source()` (= `load_profile_from_file` の再実行。apply_profile → 下流サブシステム再構成 → compiled graph 再 compile) |
| シェーダ (compiled SPIR-V `*.spv`) | **ファイル監視でホットリロード** — 変更検出 → `ShaderRegistry::rebuild_pipelines()` (custom シェーダ) / `PostProcessPipeline::rebuild_chain()` 等の既存 rebuild seam をホストが配線 |
| 値の変更 (uniform / effect パラメータ / profile 内数値のライブ調整) | **ホットリロードしない**。プログラムから既存 API で流し込む: `register_custom_profile` + `reload_active_profile`、`PostProcessPipeline::set_config`、push constant。エディタ (Ergo) は WS 経由で値を直接送る |

「値もファイル経由」にしない理由: 毎フレームに近い頻度で動く値をファイル書き込み → mtime 検出 → 全再構成で回すとレイテンシと GC 的な再構築コストが支配的になる。構造 (パス構成・シェーダ) だけをファイル境界にし、値は API 境界にする。

## 1. 構成要素

### 1.1 `FileWatchSet` (`core/file_watch.{h,cpp}`)

mtime ポーリングの変更検出。OS 監視 API・スレッドは使わない (Pictor は最下層、ホストのフレームループから `poll_changed(now_ms)` を呼ぶ)。

- 変更 = `last_write_time` の変化、または存在 ↔ 不在の遷移。
- **settle 待ち** (既定 200ms): mtime が変わってから `settle_ms` の間さらに変化がなければ確定。エディタ / シェーダコンパイラの連続書き込み中に半端なファイルを読まないため。
- 基準 mtime は登録時点。登録前の変更は報告しない。

### 1.2 `PipelineHotReload` (`pipeline/pipeline_hot_reload.{h,cpp}`)

group 名付きの監視管制。

```cpp
PipelineHotReload hot;
hot.watch(renderer.profile_source_path(), PipelineHotReload::kGroupProfile);
hot.watch_directory(shader_dir, ".spv", PipelineHotReload::kGroupShader);
hot.on_group(PipelineHotReload::kGroupProfile, [&](const auto&) {
    std::string err;
    if (!renderer.reload_profile_from_source(&err))
        log("profile reload failed: %s", err.c_str());
});
hot.on_group(PipelineHotReload::kGroupShader, [&](const auto& paths) {
    shader_registry.rebuild_pipelines();          // custom シェーダ (内部で wait idle)
    // 必要ならホスト側で postprocess rebuild_chain / compile_render_graph
});
// フレームループ (poll interval 既定 500ms で間引き):
hot.poll();
```

- コールバックは `poll()` を呼んだスレッド上で同期実行。GPU リソースを作り直す側が同期 (wait idle) に責任を持つ。
- `watch_directory` は呼んだ時点のファイル列挙。後から増えたファイルは再度呼んで追加する (エディタが新しいシェーダを足した時はホストが再スキャン)。

### 1.3 既存 seam への追加

- `PictorRenderer::profile_source_path()` — 最後に `load_profile_from_file()` で読んだパス。
- `PictorRenderer::reload_profile_from_source(error*)` — 上記パスを読み直して再適用。`set_profile / load_profile_from_file / reload_active_profile` と同じ経路なので compiled graph の再 compile も既存どおり自動。
- `ShaderRegistry::rebuild_pipelines()` — 初回 `build_pipelines()` の引数 (render pass / subpass / layout) を記憶し、SPIR-V から候補 pipeline 群を構築する。全件成功時だけ内部で `vkDeviceWaitIdle` して旧 pipeline 群と差し替える。1 本でも失敗したら候補を破棄して旧 pipeline 群を維持する = **シェーダが壊れていても last-known-good 描画を失わない**。Vulkan 無効時は同じホスト配線を使える no-op。

## 2. ホスト側の配線 (別タスク)

- KS `kuzu_visus_preview`: 既に visus JSON の mtime watch (ライブリロード) を自前実装している。`PipelineHotReload` に寄せて profile / shader も監視: `--pipeline` で渡した profile file + `shaders/` + visus の shader stages。
- KS 本体 (`GameRenderer`): `data/render/*.profile.json` と `shaders/*.spv` を watch、シェーダ変更で `SkinnedRenderer` の custom pipeline rebuild + `PostProcessPipeline::rebuild_chain()`。
- Ergo render_pipeline プラグイン (可視化): Profile Editor の保存 → ファイル → 各ホストが自動反映、の経路が成立する。値スライダは従来どおり WS で `set_config` 系に流す (ホットリロード非対象)。

## 3. 非ゴール

- GLSL → SPIR-V のコンパイル (ホスト / ビルドステップの責務。Pictor は `.spv` を読むだけ)。
- GI / decal / text 等の組み込み固定シェーダのリロード (初期化時ロードのまま。必要になったら同じ seam 方式で個別に)。
- OS ネイティブのファイル監視・監視スレッド。
- profile / shader 以外のアセット (mesh / texture) のホットリロード。

## 4. テスト

- `unit_file_watch_test`: mtime 変更検出 / settle 待ち / 不在→出現 / 変更が続く間は発火しない。
- `unit_pipeline_hot_reload_test`: group 分配 / interval 間引き / watch_directory の拡張子フィルタ / コールバック発火内容。
- ShaderRegistry rebuild は headless では no-op API 契約を unit test する。実 Vulkan の last-known-good 差し替えは実機確認 (no-run 規約)。
