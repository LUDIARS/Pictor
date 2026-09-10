---
task: android-demo-collection
project: Pictor
kind: 実装
created: 2026-09-10
memory_links:
  - spec/feature/android-demo-collection.md
  - spec/feature/android-build-validation.md
---
# Pictor単独でビルドできるAndroidデモ集APK
## 目的
最小検証ホストの方針をデモ集APKへ具体化する。端末操作はExcubitorの責務とし、Ergoへ依存しない。
## 完了条件
- APKの一覧から基本描画とsurface復旧用デモを選択する。
- 各デモのパイプライン実装と資源を分離する。
- SDK/NDKを明示してAPKを再現ビルドできる。
- 未対応機能・未確認の実機結果を完了扱いしない。
## スコープ (編集可ディレクトリ)
demo/android/、spec/feature/。既存タスク本文への進行状態書き戻しは行わない。
