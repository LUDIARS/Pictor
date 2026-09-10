---
task: android-allocator-build-compatibility
project: Pictor
kind: 実装
created: 2026-09-10
memory_links:
  - spec/feature/platform-feature-matrix.md
  - spec/data/platform-feature-matrix.json
---
# Android NDKのアロケータ互換性とライブラリビルドを修正する

## 目的
対応表の根拠となった47967d8では、Android ARM64 / API 26 / NDK 21.4.7075529 / DebugのCMake configureは成功したが、pool_allocator.cpp:103のstd::aligned_alloc未提供でpictorのコンパイルが停止した。この具体的な阻害要因を解消する。

## 完了条件
- アラインメント、確保失敗、解放APIの組み合わせを維持したAndroid対応の確保処理にする。他OSの契約を壊さない。
- 同条件でpictorライブラリのビルド成否を記録し、追加の失敗があれば原因を切り分ける。demo/tests/toolsはOFFとする。
- 対応表のAndroidメモリ管理と全体ビルド判定を実際の結果に合わせる。ビルド成功を実機動作確認と扱わない。

## スコープ (編集可ディレクトリ)
src/memory/、include/pictor/memory/、cmake/、必要時のCMakeLists.txt、spec/feature/platform-feature-matrix.md、spec/data/platform-feature-matrix.json。

## 実施制約
着手時に採用済みのnative実装を確認する。テスト・アプリ起動は本タスクの保存によって許可されたものと扱わない。
