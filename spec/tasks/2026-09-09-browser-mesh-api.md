---
task: browser-mesh-api
project: Pictor
kind: 実装
created: 2026-09-09
actio: actio:62c00573-5449-4708-a6b0-8bf04ce275ae
feature: spec/feature/browser-mesh-api.md
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

## 状況 (2026-10-15)
- 実装は `browser/` (`@ludiars/pictor-browser` 0.1.1)。仕様は [spec/feature/browser-mesh-api.md](../feature/browser-mesh-api.md)。
- mesh handleからslotを除去し、完全なopaque handleにした。
- 偽WebGL2での契約テスト `browser/test/renderer.test.mjs` を追加。構文検査のみ実施、テスト実行・ブラウザ描画確認は未実施。
