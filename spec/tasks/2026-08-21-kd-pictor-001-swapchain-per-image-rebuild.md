---
task: kd-pictor-001-swapchain-per-image-rebuild
project: Pictor
kind: 実装
status: in-progress
created: 2026-08-21
memoria_task_id: 670
memory_links:
  - ../../KonbiniDominant/spec/tasks/2026-07-31-kd-mob-002-pictor-surface-recovery.md
---

# KD-PICTOR-001 — swapchain 再生成時に per-image リソースを作り直す

## 目的

swapchain を再生成したときに swapchain image 数が変わっても、image 数に依存する
リソース (command buffer / render-finished セマフォ / images-in-flight 追跡) を
新しい個数へ安全に作り直し、consumer が配列外のリソースへ触れないようにする。

これまで `VulkanContext` は per-image リソースを `initialize()` のときにだけ確保し、
`recreate_swapchain()` では swapchain / image view / render pass / framebuffer しか
作り直していなかった。`create_sync_objects()` には「image 数は recreate_swapchain
でも不変前提」というコメントが残っており、command buffer も同じ前提でリサイズして
いなかった。present mode や surface capabilities が変わって `minImageCount` がずれ、
image 数が増えると次の 3 箇所が配列外アクセスになる。

- `acquire_next_image()` の images-in-flight 追跡 (`images_in_flight_[index]`)
- `present()` の render-finished セマフォ参照 (`render_finished_sems_[image_index]`)
- consumer 側の `command_buffers()[image_index]` (demo / KonbiniDominant の描画ループ)

## 完了条件

- `VulkanContext::recreate_swapchain()` が新しい swapchain image 数を取得したあとに、
  image 数へ依存する全リソース (command buffer、render-finished セマフォ、
  images-in-flight 追跡) を同じ個数へ作り直す。
- 旧リソースは device idle を取ったあとに生成順の逆順 (セマフォ n-1..0 →
  command buffer) で解放する。
- 新しい一式を全部作れたときだけ差し替える。途中で失敗したら新しい側だけを巻き戻し、
  既存の一式は無傷のまま残す (半端な状態を公開しない)。再生成そのものが失敗した
  場合は swapchain と per-image リソースを畳んで「空」に揃え、古い個数の配列を
  掴んだまま添字する経路を残さない。
- 起動時と再生成後で image 数が増える / 減る再現ケースをテストとして追加する。
- image 数が不変であることを前提にしたコメント・暗黙前提をコードから解消する。

## 設計

per-image リソースの所有と再構築を `PerImageResourcePool` (新規) 1 箇所へ閉じ込め、
`VulkanContext::recreate_swapchain()` は `rebuild_per_image_resources()` を 1 回
呼ぶだけにする。Vulkan API 呼び出しは backend policy `VulkanPerImageBackend` へ
分離し、テストは fake backend を渡して Vulkan device なしで再構築規則を検証する。

- `include/pictor/surface/per_image_resource_pool.h` — 個数の一致と
  replacement-before-retire の規則 (device 非依存)
- `include/pictor/surface/vulkan_per_image_backend.h` — Vulkan API 呼び出しの adapter
- `src/surface/vulkan_context.cpp` — `create_command_pool()` と
  `rebuild_per_image_resources()` へ分割し、初期化と再生成で同じ経路を通す
- `tests/unit_swapchain_per_image_pool_test.cpp` — 増減 / 逆順解放 / 失敗時の巻き戻し

## スコープ

`include/pictor/surface/`、`src/surface/`、`tests/` に限定する。
KonbiniDominant 側の `spec/interface/pictor-rendering.md` 更新と Pictor revision の
固定は、この PR が merge されたあとの後続作業。

## 実装ブランチと基点 (2026-08-21 載せ替え)

先行ブランチ `feat/kd-pictor-001-swapchain-per-image-rebuild` (commit 7985289) は
Pictor の履歴復旧作業で基点が無効になり PR 未提出のまま残っている。本タスクはその
実装内容を現在の `main` へ載せ替えたもの。

- 実装ブランチ: `feat/kd-pictor-001-swapchain-rebuild-v2`
- 基点 SHA: `70882b3e62cf6c84b8f400ef693bff1dedbd8e84`
  (`chore(release): Pictor 2.2.0 と .revisor-version を base branch に載せる`)
- 先行実装との差分: 先行基点 `c088e8d` と `70882b3` の間で本タスクの対象ファイルに
  入った変更は `engineVersion` の `2.1.0 -> 2.2.0` 1 行のみで、per-image リソースの
  取り扱いには影響しない。cherry-pick はコンフリクトなく適用できた。

## テストの実行状況

`tests/unit_swapchain_per_image_pool_test.cpp` は追加のみで **未実行** (ビルドも
行っていない)。実行にはユーザの明示指示が要る。

将来追加すべきケース (本タスクの範囲外):

- 実 Vulkan device を張って `recreate_swapchain()` を present mode 変更つきで回し、
  image 数が実際に増減する統合テスト
- `discard_swapchain_resources()` 経由で空になった状態から `recreate_swapchain()` が
  復帰できることの確認
- `present()` が stale index を弾いたあと、consumer が次フレームで正常復帰する経路

## 後続作業 (この PR の merge 後)

- KonbiniDominant `spec/interface/pictor-rendering.md` へ per-image リソースの
  再構築契約を反映する
- KonbiniDominant 側の Pictor revision pin を更新する

## Anatomia ドメイン宣言 (レビュー指摘への対応)

初回提出 (PR #865) は Anatomia review gate の `target domain is still missing` で
ブロックされた。当時の基点 `70882b3` には Pictor のドメイン宣言が 1 つも無く
(`domain-review`: domains=0 / functions=3011 / coverage=0.0%)、変更対象の anchor を
主張するドメインが存在しなかった。

その後 Revisor が本ブランチを新しい main `7460c31` へ載せ替えた。この基点には既に
24 個のドメイン宣言があり、うち `gpu-surface` (「GPU とウィンドウ表面の境界。Vulkan
インスタンス/デバイス生成、スワップチェーン、コマンド送出とリング/フライト管理」) が
`src/surface/[^/]+` と `include/pictor/surface/[^/]+` を既に主張している。本タスクの
変更はまさにこの括りに収まるため、**新しいドメインは作らない**。

一度は `surface-presentation` を追加したが、`gpu-surface` と二重所属
(`domainOverlap = 2`) になるため取り下げた。代わりに `gpu-surface` の membership の
テスト列挙へ本タスクのテストを 1 件足すだけにしている。

- 変更: `(^|/)tests/unit_(?:gpu_ring_flight|gpu_timer)_test\.cpp$`
  → `(^|/)tests/unit_(?:gpu_ring_flight|gpu_timer|swapchain_per_image_pool)_test\.cpp$`

これは `test-support` ドメインが自身の説明で述べている「個々のテストは検証対象の
ドメインに属する」という既存の規約に沿う。結果は `hasTargetDomain=true` /
未割り当て anchor 0 で、ドメインを 1 つ増やした場合と同じ。

残る非ブロック所見と、それを本 PR で扱わない理由:

- **`convention_drift` ゲートの warn**: `PerImageResourcePool` / `VulkanPerImageBackend`
  の型名 (pascal) と `rebuild` / `destroy` / `empty` などのメソッド名が、Anatomia の
  sibling 推定 (snake) と食い違うと報告される。実際には Pictor 既存コード
  (`VulkanContext::recreate_swapchain()` 等) と同じ「型は PascalCase / メンバ関数は
  snake_case」規約に従っており、`empty` / `destroy` / `rebuild` は 1 語なので
  snake_case としても正しい。誤検知として扱い、命名は変更しない。
- **dual-layer (program) の未分類 anchor**: program 層のドメイン定義 (layers.json)
  は Pictor にまだ無く、リポジトリ全体の課題。advisory (`blocking: false`) であり、
  surface だけ入れても解消しないため別タスクとする。
