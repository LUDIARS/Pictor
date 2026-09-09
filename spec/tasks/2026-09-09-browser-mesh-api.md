---
task: browser-mesh-api
project: Pictor
kind: 実装
created: 2026-09-09
memory_links:
  - spec/setup/integration.md
---
# ブラウザconsumer向けメッシュ描画API

## 目的
ブラウザアプリが独自GLコードを持たず、Pictorの公開APIで頂点色メッシュと変換行列を描画する。

## 完了条件
- Pictorがコンテキスト・プログラム・GPUメッシュの確保と解放を所有する。
- consumerにはGPUハンドルを露出せず、頂点色メッシュ、モデル行列、view-projection行列、フレーム境界を提供する。
- 独立したバージョン付きパッケージとして取得でき、Pagus型・定数・ファイルへ依存しない。
- 既存C++ WebGLデモへの偽装や未実装APIのフォールバックをしない。

## スコープ (編集可ディレクトリ)
browser/、spec/feature/、spec/tasks/。テスト実行・サービス起動は含めない。
