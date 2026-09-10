---
task: android-surface-recovery-verification
project: Pictor
kind: テスト
created: 2026-09-10
memory_links:
  - spec/feature/platform-feature-matrix.md
  - spec/tasks/2026-09-10-android-allocator-build-compatibility.md
---
# Android実機の描画とsurface復旧を確認する

## 目的
対応表で未確認のAndroid実機描画、背景復帰、surface/device復旧について、実装の存在と動作の証拠を区別する。

## 完了条件
- Androidビルド成立後、対象端末・OS・GPU・採用commit・使用モジュールを記録する。
- 初回描画、背景移行と復帰、surface破棄と再生成について結果を記録する。device lossは安全に再現できる手段を確認し、未実施なら未確認のまま明記する。
- 描画再開、不要な再初期化、リソースの再確保や残留の観察結果を残す。
- 対応表に確認した構成と機能だけ反映する。nativeの全機能統合を条件にしない。

## スコープ (編集可ディレクトリ)
spec/feature/platform-feature-matrix.md、spec/data/platform-feature-matrix.json、review/の確認記録。実装修正が必要なら原因と修正範囲を別タスクへ分離する。

## 実施制約
実機・実行環境の確保とユーザーの明示的なテスト許可を前提とする。起動操作はExcubitor経由でプロジェクト本体から行い、事前にConcordiaへtesting claim、終了時にreleaseする。worktreeや複製から起動しない。
