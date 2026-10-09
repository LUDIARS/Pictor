---
task: frame-tap-render-tap-1
project: Pictor
kind: 実装
created: 2026-10-09
actio: actio:bf517601-b762-4891-b070-fa28e971a45d
previous_actio:
  - actio:e04e2cc9-cafb-476e-909e-339b0246300b
  - actio:c3cc8d7c-1d46-4d60-a06b-c3f8d8ee3b7e
parent_actio: actio:7ab9fce8-4e31-4a97-9fe7-62db95bea1f7
memory_links:
  - spec/feature/frame-tap.md
  - spec/tasks/2026-10-09-frame-tap.md
---
# 段階 6P 続き: フレームタップを描画タップ契約 render-tap/1 に合わせる

## 目的

Commentarii で確定した描画タップ契約 render-tap/1 (Commentarii main 4c72a7b の
`spec/feature/render-tap-contract.md` / `schema/render-frame.schema.json` /
`tests/fixtures/render-tap/golden-v1.jsonl`) に、Pictor のフレームタップ
(`spec/tasks/2026-10-09-frame-tap.md` で入れたもの) の出力を合わせる。Commentarii は読むだけで編集しない。

## 完了条件

前タスクの C-1〜C-8 を引き継ぎ (C-5〜C-8 は契約の形に更新)、契約の追加分を C-8〜C-17 とする。

- C-1 parse_frame_tap_setting(value): 未設定 / 空 / `0` / `off` は無効、`stdout` / `-` は標準出力、それ以外はファイルパス
- C-2 make_frame_tap_sink(setting, error): ファイルを開けないときは null と理由を返し、無言で別経路へ逃げない
- C-3 project_world_aabb_to_screen(bounds, view_proj, viewport, out): 8 隅の投影の外接矩形を px (左上原点) で返し、視点の後ろにまたがる箱は w = ε 面で切ってから射影する
- C-4 resolve_mesh_id(info): 名前 → asset-name、内容ハッシュ → `ch:` + 16 桁 hex で content-hash、どちらも無ければ `fp:` + 頂点数とインデックス数の FNV-1a 64bit で count-hash を返す
- C-5 encode_frame_tap_line(frame, out): 改行を含まない 1 行で contract / seq / frame / tick? / t / observer / camera / visibility_lag_frames? / passes[{name, kind, draws}] / dropped? と、draw ごとの mesh, material[], identity, instance, generation, world, screen_bbox, depth_order, visibility, alpha?, clip?, ui?, tags[] を出す。end 行は `{contract, seq, end}`
- C-6 FrameTap::end_frame(): scene 系は視点深度の近い順、ui は提出順 (0 = 最初に描いた) に depth_order を振り、配送先へ 1 行渡す
- C-7 FrameTap::is_enabled(): 配送先が無ければ false で何も出さない。無効化は end 行 (shutdown) で流れを閉じる
- C-8 FrameTap (流れ): seq はタップが開いたフレームごとに +1 (捨てたフレームも数える)、配送先の差し替え / 無効化 / 破棄で end 行 shutdown、end_stream(FAILED) で end 行 error、新しい流れは seq 0 から
- C-9 FrameTap::set_observer_id(id): 既定 `player-camera`、書式 `^[a-z0-9][a-z0-9_.-]*$` に合わない ID は拒否し、行の observer {id, viewport} に出す
- C-10 FrameTapClock::next_time / next_tick: ホストのゲーム時刻・tick を優先し、無ければ delta 累積を使う。有限でない・負・逆行は前の値に留め、tick はホストが与えたときだけ出す
- C-11 FrameTap::add_scene_draw (identity): 名前の無いマテリアルはハンドル番号を出さず identity を count-hash に下げ、名前の無いメッシュは unnamed タグを付ける
- C-12 FrameTapGenerations::observe(space, instance, appearance): 初見 0、外見 (mesh + 並べ替えた material[]) が変われば +1。ui の instance は 2^32 を足して scene と重ねない
- C-13 FrameTap (dropped): 有限でない / 契約の範囲外 / 存在しない pass の draw は出さず dropped (other)、上限超えは buffer-full。行に null を出さない
- C-14 FrameTap::add_ui_draw (UI の意味): alpha (0..1 に丸める)、clip、ui {bar: fill} / {glyph: glyph} を出し、空の glyph は ui だけ落とす
- C-15 FrameTap::add_pass(name, kind): scene / ui の後に任意の kind (shadow / reflection 等) の pass を 1 フレーム限りで足せる
- C-16 collect_scene_frame_tap (可視性の根拠): 口が無ければ frustum-only で visibility_lag_frames を出さず、口があればオブジェクトごとの根拠と lag を出す
- C-17 PictorRenderer (固定シーン: メッシュ 2 + UI 矩形 1 + 影 pass 1): タップ ON で 2 フレーム + end 行が契約の形で出て schema に適合し、OFF で何も出ず描画統計が変わらない。名前の無いメッシュはタップ ON の登録時だけ content-hash になる

## 再利用探索

- 前タスクの FrameTap 一式 (sink / config / projection / name table / json line): 採用して拡張。出力形式だけを契約へ寄せ、
  有効化経路・配送先・投影は変えない。
- `frame_tap_mesh_fingerprint` の FNV-1a: 採用 (内容ハッシュ・外見のハッシュも同じ FNV-1a 64bit の混ぜ方で書いた)。
- Commentarii の `src/render-tap/render-frame-sequence.ts`: 読むだけ。受信側が instance を pass 横断で照合し、
  同じ generation の外見変化を違反とすることを確認し、ui の instance を 2^32 ずらす判断と、外見変化で世代を上げる判断の根拠にした。
- Pictor の遮蔽カリング: 不採用 (使えない)。`CullingSystem` の Level 2 / 3 (CPU ソフト遮蔽 / GPU Hi-Z) は CPU から読める
  遮蔽結果を持たない。代わりに `IFrameTapVisibilitySource` を足し、既定は frustum-only とした。
- Pictor の影 pass: 不採用 (使えない)。`RenderPassScheduler` の ShadowPass は managed 経路で draw 単位の CPU 記録を持たないため、
  ホストが `add_pass()` で記録する口にした。
- `DataQueryAPI::get_mesh_info`: 前タスクと同じ理由 (線形探索) で不採用。内容ハッシュは登録時に名前表へ写す。
- Commentarii の schema 検証: Ajv を Commentarii の node_modules から借りる開発用スクリプト (`tools/frame_tap/validate-render-tap.mjs`)
  にした。Pictor に node 依存は足さない。
- Augur の `contract-wrap`: TypeScript / JavaScript 専用で C++ に注入できず、Pictor に `augur.contracts.json` も無い。
  契約 C-n は `unit_frame_tap_test` のアサーションで判定する (前タスクと同じ)。

## 変更した境界

- 出力形式: 全行に `contract` / `seq`、`camera.viewport` → `observer {id, viewport}`、`passes[].pass` → `{name, kind}`、
  draw に `identity` / `generation` / `visibility` / `alpha` / `clip` / `ui`、任意 `tick` / `visibility_lag_frames` / `dropped`、end 行。
  ui の `instance` は 2^32 を足した値、ui の `depth_order` は提出順 (0 = 最初) に変えた。有限でない値は `null` ではなく dropped。
- `FrameTap`: `set_observer_id` / `clock()` / `add_pass` / `set_visibility_source` / `set_max_draws_per_frame` / `end_stream` /
  `dropped_frames` を追加。`set_sink` / `disable` / 破棄は end 行を出す。`begin_frame` の第 2 引数は「代わりの t」。
- 公開型: `FrameTapPass` / `frame_tap_pass_name` を廃止し、`frame_tap_vocabulary.h` (契約の列挙と文字列) に置き換えた。
  `resolve_mesh_id` は bool ではなく `FrameTapIdentity` を返す。`FrameTapMeshInfo` に内容ハッシュを足した。
- 新規公開ヘッダ: `frame_tap_vocabulary.h` / `frame_tap_clock.h` / `frame_tap_generations.h` / `frame_tap_visibility_source.h`。
- `UIRenderer`: `set_frame_tap_annotations()` を追加。`record()` はタップ ON のとき clip / alpha / 意味を記録する。
- `PictorRenderer`: 名前の無いメッシュをタップ ON で登録したとき内容ハッシュを計算する。`shutdown()` は end 行を出す。
- 新規: `tools/frame_tap/validate-render-tap.mjs` (開発用)。ドメイン定義 (graphics-domain-map / profiling.domain) にパスを足した。

## 復旧方法

- タップは観測専用で描画結果を変えない。不具合時は `PICTOR_FRAME_TAP` を外す (または `RendererConfig::frame_tap` を空 / `"off"`)
  と、OFF 時の分岐 1 つ以外は変更前と同じ経路になる (OFF 時は UIRenderer の PopClip で bool を 1 つ書くだけ増えた)。
- 戻す場合は本 PR を revert する。前タスクの出力形式 (契約前の暫定形) に戻るが、Commentarii の受信側は render-tap/1 を前提とする。

## 検証

- 実施: Windows / MSVC 2022 (14.39) Debug で `pictor` と全テストをビルド (エラーなし。タップ由来の新しい警告なし)。
- 実施: `unit_frame_tap_test` Pass (C-1〜C-17)。
- 実施: ctest 全 46 件中 44 件 Pass。失敗 2 件 (`unit_visus_resource_loader_test` の絶対パス文言、
  `unit_compiled_graph_wiring_test` の再コンパイル) はタップと無関係で、前タスクの記録でも変更前の HEAD で失敗している。
- 実施: 固定シーンの出力 (2 フレーム + end 行) と golden を `tools/frame_tap/validate-render-tap.mjs` で Commentarii の
  schema (4c72a7b) に照らし、両方適合・流れの形も一致。`fill` を null にした負例は不適合と判定されることも確認。
- 未実施: GCC でのビルドとテスト。手元の WSL (Ubuntu) は仮想ディスクのマウントに失敗 (`Wsl/Service/CreateInstance/MountDisk/0x800701c0`)、
  Docker デーモンも停止しており、GCC が無い。GCC で問題になりやすい箇所 (`_dupenv_s` / `fopen_s` は `_MSC_VER` 限定、
  浮動小数点 `to_chars` は GCC 11 以降、`UINT32_MAX` は `<cstdint>`、`wingdi.h` の `ERROR` マクロを避けた列挙子名) は目視で確認した。
- 未実施: UIRenderer::record() 経由の clip / 意味付けの実機確認 (Vulkan デバイスが要る)。テストは record() が呼ぶのと同じ
  `FrameTap::add_ui_draw()` を直接呼んで確認した。
