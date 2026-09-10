---
task: android-validation-host
project: Pictor
kind: 実装
created: 2026-09-10
memory_links:
  - spec/feature/android-build-validation.md
  - spec/tasks/2026-09-10-android-surface-recovery-verification.md
  - spec/architecture/framework-boundaries.md
---
# Android実機検証用の最小ホストアプリを用意する

## 目的
静的ライブラリのビルド成功から実機描画・復旧確認へ進むため、Pictorをリンクして起動できるAndroidホストを用意する。確認記録ではAndroidManifest.xml、Gradleプロジェクト、APKが見当たらず、ホストが検証の前提として不足している。

## 完了条件
- 着手時に最新の採用ソースと既存Androidホストの有無を確認し、利用できるものがあれば重複して作らない。
- 必要なモジュールのみをリンクする最小の検証アプリを構成する。Manifest、Androidビルド設定、必要なJNIまたはNativeActivity接続を用意し、方式は既存のsurface APIに合わせる。
- Androidのwindow生成・破棄、pause/resume、サイズ変更を既存のsurface/lifecycle APIへ伝える。ANativeWindow等の所有・解放を明確にする。
- 初回描画と復旧前後のフレーム状態を確認できる最小描画内容・診断情報を用意する。未対応のGPU/API条件は明示的に診断する。
- 対象ABI/API/NDK、使用モジュール、採用commitを特定できるAPKをビルドし、最終リンクの結果と再現手順を記録する。
- 実機テストは既存のandroid-surface-recovery-verificationタスクへ引き渡す。APK生成だけで実機確認済みにしない。

## スコープ (編集可ディレクトリ)
検証アプリ用のdemo/android/（既存配置があればそこを使用）、cmake/、CMakeLists.txt、docs/、spec/feature/。Pictorのsurface APIを読むが、コア描画や復旧処理そのものの変更が必要なら原因と範囲を別途切り分ける。

## 制約
検証アプリ固有のパイプラインとし、他demoとの共有化やnative全機能統合を行わない。ゲームロジックやErgoへの依存をPictorへ持ち込まない。実機の使用条件はユーザー回答から確認し、端末や接続情報を推測しない。
