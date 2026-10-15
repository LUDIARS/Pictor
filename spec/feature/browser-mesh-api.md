# ブラウザネイティブ・メッシュ描画API

`browser/` の `@ludiars/pictor-browser` は依存なしのJavaScript/WebGL2バックエンド。C++ WebGL参照デモとは別の公開経路であり、WASMのロードを装わない。上位のゲーム型やファイルへの依存を持たない。

- PictorRendererがHTMLCanvasElementからコンテキストを取得し、シェーダとGPUバッファを所有する。
- createMeshはxyz/rgbの三角形リストを受け、opaque meshを返す。meshは自身のプロパティを持たない凍結オブジェクトで、slotやGPUバッファはrenderer内部のWeakMapからのみ引く。他rendererのmesh・解放済みmesh・形だけ真似たオブジェクトは拒否する。
- beginFrameで物理ピクセル寸法・column-major viewProjection・背景色を受ける。
- submitはObjectDescriptor（mesh/transform）を描画する。上位はGL handleやGLSLを持たない。
- endFrameでフレームを終了し、disposeは全GPU資源を解放する。部分的な初期化失敗も確保済み資源を解放する。
- 不正な数値・データ・重複beginFrame・フレーム外submit/endFrame・context lossは明示エラーにする。
- 0.1.0は頂点色・深度・方向光に限定。テクスチャ、スキニング、PBR、WASM統合を提供したとは扱わない。

配布はbrowser/package.jsonの固定バージョンtarball (0.1.1でmesh handleからslotを除去)。consumerはlockfileでintegrityを固定する。

## テスト

`browser/test/renderer.test.mjs` (`npm test`) が偽WebGL2コンテキストに対して次を検査する。実GPU・ブラウザでの描画は検査対象外。

- program/バッファの確保と、dispose・部分初期化失敗での全解放、context解放が1回であること
- mesh handleが空の凍結オブジェクトで、偽造・他renderer・解放済みhandleを拒否すること
- beginFrame→submit→endFrameの呼出順、フレーム外・二重beginFrameの拒否、不正値・context lossの明示エラー
- package.jsonが固定バージョンで依存を持たず、配布ファイルがPagus・WASM・Emscriptenを参照しないこと

関連タスク: actio:62c00573-5449-4708-a6b0-8bf04ce275ae ([spec/tasks/2026-09-09-browser-mesh-api.md](../tasks/2026-09-09-browser-mesh-api.md))
