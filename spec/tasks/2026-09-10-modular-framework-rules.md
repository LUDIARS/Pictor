---
task: modular-framework-rules
project: Pictor
kind: 仕様
status: implemented
created: 2026-09-10
---

# ゲーム非依存・任意モジュール・demo既定パイプラインをルール化する

## 実装内容

necoの2026-09-10指示を `spec/architecture/framework-boundaries.md` に正本として保存。
AGENTS.mdとCLAUDE.mdから参照し、今後の設計・実装・レビューに適用する。
Pictorのゲーム非依存、SHaRC等の除外契約、各描画demoとconsumerが共有するpresetを規定する。

## 受け入れ条件

- PictorからErgoやゲーム状態への必須依存を認めない。
- 不要機能を外す際のbuild/link/shader/実行時資源の境界を定義する。
- demoの実パイプラインと品質profileを区別し、既定値を二重管理しない。
- 現状と移行作業を明記し、ルール追加をモジュール実装の完了にしない。

## 検証と残作業

参照先と差分の読み取り確認のみ。ユーザー指示に従いテスト・ビルド・起動は未実行。
このPRはルール整備。機能モジュール分割と全demoの共有preset移行は正本末尾の手順で実装する。
既存native backend PR #1640のマージ・変更は行わない。
