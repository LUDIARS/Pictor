---
task: demo-platform-matrix
project: Pictor
kind: 雑用
created: 2026-09-10
memory_links:
  - spec/feature/platform-feature-matrix.md
  - spec/data/platform-feature-matrix.json
  - spec/architecture/framework-boundaries.md
---
# 各demo固有の描画機能とプラットフォーム対応を整理する

## 目的
既存対応表でTOOLS-01へまとめた毛皮・縄・涙・影絵などのdemo専用表現を、demo単位の対応情報へ展開する。

## 完了条件
- CMakeのdemo targetと実装を照合し、各demoの使用機能、モジュール依存、固有パイプライン、対応バックエンドを記録する。
- ライブラリの汎用機能とdemo専用表現を区別し、既存の機能IDに関連付ける。
- ソース経路、ビルド確認、実機確認を別々に表示し、未確認や条件付き対応を明記する。
- 各demoは独立した設計として記載する。デフォルトとして提供する場合もdemoごとの選択肢であり、共有の万能パイプラインへ統合しない。

## スコープ (編集可ディレクトリ)
spec/feature/、spec/data/。CMakeLists.txt、demo関連ソースは読み取り調査対象。

## 実施制約
対応表の作成だけを行い、描画モジュールの移植や起動テストを含めない。Pictor本体へゲーム依存の実装を追加しない。
