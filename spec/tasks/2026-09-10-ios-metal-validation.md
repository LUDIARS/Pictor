---
task: ios-metal-validation
project: Pictor
kind: テスト
created: 2026-09-10
memory_links:
  - spec/feature/platform-feature-matrix.md
  - spec/data/platform-feature-matrix.json
---
# 環境確保後にiOS Metalのビルドと基本描画を確認する

## 目的
iOS Metalの実装経路は対応表にあるが、SDKビルドと実機確認は未実施である。環境を確保できた段階で基本描画と復旧の証拠を追加する。

## 完了条件
- 対応表の参照先47967d8と採用先の差分を確認し、Metal実装の二重実装を避ける。
- Xcode・SDK・端末・採用commitを記録し、必要なモジュールのみ選択してビルドする。
- 基本メッシュ描画、背景復帰、描画surfaceの再生成について実施結果を残す。未実施項目は対応済みにしない。
- 対応表のiOS欄を確認範囲に限定して更新する。Vulkan効果の一括移植やnative全機能統合は含めない。

## スコープ (編集可ディレクトリ)
spec/feature/platform-feature-matrix.md、spec/data/platform-feature-matrix.json、review/の確認記録。

## 実施制約
iOSを確認できる環境とユーザーの明示的なテスト許可が必要。現在確認できないことを理由に動作済みと推定しない。起動はプロジェクト本体からExcubitor経由とし、Concordiaのtesting claim/releaseを行う。利用可能な実行経路がなければ未実施として扱う。
