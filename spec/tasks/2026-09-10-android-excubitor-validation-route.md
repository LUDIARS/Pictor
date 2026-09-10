---
task: android-excubitor-validation-route
project: Pictor
kind: 実装
created: 2026-09-10
memory_links:
  - spec/feature/android-build-validation.md
  - spec/tasks/2026-09-10-android-validation-host.md
  - spec/tasks/2026-09-10-android-surface-recovery-verification.md
---
# Android検証アプリのExcubitor操作経路を用意する

## 目的
実機確認をプロジェクト本体からExcubitor経由で行えるようにする。確認記録ではPictorのAndroid/ADB用サービス定義が見当たらないため、端末操作の前提を明示して整備する。

## 完了条件
- Excubitorの現行APIと既存の端末操作能力を確認し、再利用できる経路を優先する。
- 対象端末の接続先・USBデバッグ許可をユーザー回答で確定する。複数端末時の暗黙選択、無関係なアプリ停止や端末初期化を行わない。
- Pictor所有のcatalogと必要最小限の操作スクリプトを用意し、cwdがPictor本体を指すことを検証する。worktreeや複製からの起動を拒否する。
- SDK/ADB、APK、対象package/activity、端末接続・認証を入口で確認し、不足時は診断を返す。端末固有値はソースへ固定しない。
- 検証アプリの導入・起動・終了と必要な診断収集の操作範囲を文書化する。自分が開始した処理や取得した資源の終了方法を設け、共有ADBサーバーや他セッションを無条件に停止しない。
- Concordiaのtesting claim、競合確認、Excubitor操作、実状態確認、finally相当のreleaseを手順化する。起動受付だけで描画成功を判定しない。
- 実機での描画・背景復帰・surface復旧の成否判定は既存のandroid-surface-recovery-verificationタスクで行う。

## スコープ (編集可ディレクトリ)
Pictorのexcubitor.catalog.yaml、tools/android/、docs/、spec/feature/。既存catalogの他サービス定義を保全する。ExcubitorやConcordia自体の変更が必要なら当該リポの別タスクへ切り分ける。

## 依存と制約
android-validation-hostで用意するAPKと、ユーザーが指定する端末を前提とする。端末情報の回答待ちは維持する。このタスクの新規保存だけをサービス起動やテストの追加許可と解釈しない。
