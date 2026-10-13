# 設計依頼: 描画方式の切り替え口 (RHI) — Vulkan / DirectX 12 / Metal / WebGPU / WebGL2

- 日付: 2026-10-14
- 対象: Pictor (Pc)
- Actio: `a38aab5b-3141-4093-a108-a71722bd25b0` (Ludellus-Native の Native 一本化 段階 1「描画方式の切り替え口」)

## 0. 進め方 (二本立て)

この依頼文は Fable と Astra の 2 者へ同じ内容で別々に渡している。**もう一方の案は見ないこと。**
親セッションが 2 案を突き合わせて統合設計を 1 本にする。あなたの役目は独立した 1 案を書くこと。

## 1. 人間の指示 (原文)

- neco 2026-10-14:「あとPictorのレンダリングAPIにDirectX、Metal、WebGPU/GLの対応入れる」
- neco 2026-10-14:「これはLnのActioのタスクでも言われてたやつ。」
- neco 2026-10-14:「描画は抽象化してそれぞれのプラットフォームでなんも考えなくていいように。」
- Ludellus-Native 側の決定 (neco 2026-10-14、`E:/Document/Ars/Ludellus-Native/spec/tasks/2026-10-14-native-first-web.md`):
  「Web とアプリで同じことやるの無駄だから、 PC 版の C++ コードをベースに Web 版は同じコードを Webasm にして配信がいいな」
  描画の PC / Web 差は Pictor が吸収する。Web の描画方式は WebGPU を主、WebGL2 を予備。

## 2. 親の解釈 (違うと思えば指摘すること)

- Pictor に描画方式の切り替え口 (RHI) を作り、Vulkan を口の内側へ移し、DirectX 12・Metal・WebGPU・WebGL2 を
  同じ口に並べる。「GL」は Web の予備としての WebGL2 (= OpenGL ES 3.0 相当) と解釈する。デスクトップ OpenGL は
  要否を論点として扱う (推奨つきで)。
- 「それぞれのプラットフォームでなんも考えなくていい」= **消費側 (ゲーム・Ergo) のコードは描画方式を一切知らない**。
  同じ消費側コードが、方式の選択 (ビルド構成または実行時の自動選択) だけで全方式で動く。方式固有の型・分岐・
  シェーダの書き分けを消費側に要求しない。能力差による降格も Pictor が判断し、消費側は結果 (どの方式・何を降格したか)
  を読めるだけ。これを設計の最上位の受入条件とする。
- 既存の DX12 設計 (`spec/feature/dx12-backend-design.md`) は「先に Vulkan を RHI に載せ替え、その上に DX12」の
  段階計画を持つ。これを 5 方式に広げた設計として作り直すのが今回の範囲。
- DX12 / Metal の描画面 (surface/present) と復旧経路は `feat/native-metal-dx12-recovery` ブランチに実装があるが
  main 未反映 (main との差分は 547 ファイルと大きく、無関係な変更も含む)。これを取り込むか・必要部分だけ移すかも論点。
- 消費側の当面の主目的は Ludellus-Native の WebAssembly 化 (WebGPU 主 / WebGL2 予備) なので、段階の優先順位は
  Vulkan の口への移設 → WebGPU → WebGL2 → DX12 / Metal の順が自然と考えるが、独立に判断してよい。

## 3. 前提として読むもの (すべて絶対パス)

Pictor (main と同内容の作業場所 `E:/Document/Ars/Pictor-design-multi-rhi`):
- `AGENTS.md`、`CLAUDE.md`、`spec/architecture/framework-boundaries.md` (ゲーム非依存・任意モジュール・demo と共有する既定値)
- `spec/feature/dx12-backend-design.md`、`spec/feature/metal-backend-design.md`
- `spec/feature/platform-feature-matrix.md` (機能 × 方式の現状。「実装あり」は動作保証ではない)
- `spec/feature/portability.md`、`spec/feature/portability/web.md`、`spec/feature/portability/mobile.md`、`spec/feature/subsystem/surface.md`
- `include/pictor/surface/` (vulkan_context / dx12_context / metal_context / surface_provider / frame_result)
- `include/pictor/webgl/` (別実装の WebGL2。`webgl_renderer.h` の RenderBackend は「予定」のまま)
- `include/pictor/pipeline/` (PipelineProfileDef → CompiledGraph → RenderPassScheduler。CompiledPass は Vk ハンドル直値)
- `include/pictor/profiler/` (gpu_timer / batch_gpu_timer は VkQueryPool)
- `include/pictor/core/pictor_renderer.h`、`include/pictor/c_api.h`
- `shaders/` (GLSL → SPIR-V の現状)
- ブランチ `feat/native-metal-dx12-recovery` (`git log main..feat/native-metal-dx12-recovery`、`git show` で読む)

消費側:
- `E:/Document/Ars/Ludellus-Native/spec/feature/native-first-web.md`、同 `spec/tasks/2026-10-14-native-first-web.md`
  (Ludellus-Native は今 `render/vk_resources` などで Vulkan を直接呼んでいる。これを無くすのが段階 2 以降の前提)
- Ergo の `ergo_render` (`E:/Document/Ars/Ergo/include/ergo/render/`): FrameComposer / IRenderLayer が VkRenderPass /
  VkCommandBuffer を公開 API で受けている。Ergo の `ergo_gpu_particle` は `IGpuBackend` で Vulkan / WebGPU 両対応の前例。

## 4. 設計してほしいこと (すべて扱う。独立に考えてよく、親の解釈と違う結論でもよい)

1. **RHI の境界**: どこで切るか (デバイス・スワップチェーン・コマンド記録・リソース・パイプライン・クエリ・
   バインディング)。インターフェースの粒度、仮想関数か静的選択 (コンパイル時) か実行時選択か、hot path の
   コスト (CompiledPass の不変条件: hot path に string / map を持ち込まない) との両立。
   **消費側が方式を知らずに済む公開面** (何を公開し、何を Pictor の内側に閉じるか) を明示する。
2. **5 方式の能力差の扱い**: compute の有無 (WebGL2 は無い)、GPU-driven / indirect draw count、timestamp クエリ、
   storage buffer、bindless、MSAA、render pass の明示 / 暗黙 (Metal / WebGPU / D3D12 の違い)。能力照会の型と、
   能力が無いときの降格規則 (黙って劣化させず、事実を記録と画面に出す)。降格を消費側に書かせない形。
3. **バインディングモデルの共通化**: Vulkan descriptor set / D3D12 root signature + descriptor heap /
   Metal argument buffer / WebGPU bind group / WebGL2 uniform・texture unit の対応と、Pictor の資源宣言側の形。
4. **シェーダ戦略**: 正本言語 (GLSL / HLSL / Slang / WGSL 等) と変換経路 (SPIR-V / DXIL / MSL / WGSL / GLSL ES)、
   ビルド時生成か実行時か、Visus shader package (`include/pictor/visus/visus_shader_package.h`) との関係、
   既存 GLSL 資産の移行量。消費側のカスタムシェーダも 1 ソースで全方式に出る形。
5. **パイプライン系 (PipelineProfile → CompiledGraph) の RHI 化**: attachment / render pass / framebuffer の抽象、
   CompiledPass から Vk 型を外す形、host_recorded パスと post-process の扱い。
6. **ウィンドウ / 表示面**: surface_provider の各実装 (GLFW / Android / iOS / Web canvas) と各方式のスワップチェーンの
   対応、FrameResult による復旧 (デバイス喪失・surface 喪失) を方式共通にする形。方式の自動選択規則 (既定と上書き)。
7. **WebGPU / WebGL2 の実装形態**: Emscripten (emdawnwebgpu など) か、ネイティブでも Dawn / wgpu-native を使うか、
   既存 `pictor_webgl` 別実装を口の内側へ統合するか捨てるか。Ludellus-Native の wasm 化に必要な最低条件。
8. **DX12 / Metal の既存実装の扱い**: `feat/native-metal-dx12-recovery` の取り込み方 (全体マージ / 必要部分の移植 /
   作り直し) と根拠。
9. **公開 API と消費側**: `PictorRenderer` / `c_api.h` / Ergo `ergo_render` (Vk 型を公開している) をどう移行させるか。
   互換期間の有無。Pictor から Ergo への依存は作らない (AGENTS.md)。
10. **ビルド構成**: CMake のオプション (方式ごとの ON/OFF、既定値)、依存 SDK の取り扱い (取り込まない / 暗黙 fetch しない
    既存方針との関係)、CI (GCC が通ること。MSVC で通って GCC で落ちる前例あり)。
11. **段階計画**: 各段階が単独で main に入れられ、既存の Vulkan 経路が退行しない分け方。各段階の受入条件
    (何をテストし、何を実機で確かめるか。実機未確認は未確認と書く前提)。「消費側が方式を知らない」ことを
    機械的に確かめる検査 (例: 公開ヘッダに方式固有の型が 0 件) を含める。
12. **リスクと未決事項**: 推奨つきで。人間 (neco) にしか決められない点は分けて書く。

## 5. 成果物

- 出力先 (自分の側だけ):
  - Fable: `spec/plan/2026-10-14-multi-rhi-fable.md`
  - Astra: `spec/plan/2026-10-14-multi-rhi-astra.md`
- 章立て: 結論 → 解釈 (親の解釈への同意・異論) → 比較した案 → 推奨案の詳細 (上の 1〜10) → 段階計画と受入条件 (11)
  → リスク → 未決事項 (推奨つき) (12)。
- **実装しない。設計書以外のファイルを変更しない。**
- 書き終えたらコミットし、要点を 5 行で報告して、終了の指示を待つ。**PR は出さない。**
- Pictor は公開リポジトリ。非公開タイトル名・社内の固有名を設計書に書かない (Ludellus は公開済みの LUDIARS プロジェクトなので可)。
