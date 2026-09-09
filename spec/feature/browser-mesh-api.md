# ブラウザネイティブ・メッシュ描画API

`browser/` の `@ludiars/pictor-browser` は依存なしのJavaScript/WebGL2バックエンド。C++ WebGL参照デモとは別の公開経路であり、WASMのロードを装わない。上位のゲーム型やファイルへの依存を持たない。

- PictorRendererがHTMLCanvasElementからコンテキストを取得し、シェーダとGPUバッファを所有する。
- createMeshはxyz/rgbの三角形リストを受け、opaque meshを返す。他rendererのmeshや解放済みmeshは拒否する。
- beginFrameで物理ピクセル寸法・column-major viewProjection・背景色を受ける。
- submitはObjectDescriptor（mesh/transform）を描画する。上位はGL handleやGLSLを持たない。
- endFrameでフレームを終了し、disposeは全GPU資源を解放する。部分的な初期化失敗も確保済み資源を解放する。
- 不正な数値・データ・重複beginFrame・フレーム外submit/endFrame・context lossは明示エラーにする。
- 0.1.0は頂点色・深度・方向光に限定。テクスチャ、スキニング、PBR、WASM統合を提供したとは扱わない。

配布はbrowser/package.jsonの固定バージョンtarball。consumerはlockfileでintegrityを固定する。構文検査は実施、ブラウザ起動・描画テストは未実施。
