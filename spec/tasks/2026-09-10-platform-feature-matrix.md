---
task: platform-feature-matrix
project: Pictor
kind: 仕様
status: implemented
created: 2026-09-10
---

# 機能別のプラットフォーム対応表を作成する

## 実装内容

登録済み41機能とIntel PCM計測の計42項目を、7つのOS/GPU API構成で整理する。
正本は`spec/feature/platform-feature-matrix.md`と`spec/data/platform-feature-matrix.json`。
CPU処理、GPU実装、host配線、別ブランチ実装、ビルド・実機確認を区別する。

## 受け入れ条件

- Windows Vulkan/DX12、Android Vulkan、iOS Metal、WebGL2を混同しない。
- Android ARM64/API 26/NDK r21の実際のコンパイル失敗を記録する。
- iOS未検証、GPU-driven等の部分実装、Webの独立最小rendererを明記する。
- 各機能にソース根拠を付け、別ブランチの実装をmainへ反映済みと扱わない。
- 全backendでの全機能一体化を対応完了の条件にしない。

## 確認

ソース・機能一覧・過去のビルドログの読み合わせと差分確認のみ。
この資料作成では新たなビルド・テスト・起動を実行していない。
