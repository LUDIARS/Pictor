# SPEC-PC-WEB-MESH — Web 描画モジュール (WebAssembly + WebGL2)

2026-10-09 neco 指示: ブラウザで動く上位 (three.js などで 3D を描いているもの) の描画基盤を
Pictor の WebAssembly へ移せるよう、ゲーム非依存の Web 描画モジュールを Pictor に整える。

## 提供するもの

添字付き三角形メッシュ (頂点ごとの位置・法線・色) を、半球環境光と最大 2 本の平行光で描く。
材質はレンダラ単位の metal / rough 1 種類 (Lambert 拡散 = albedo / π、平行光は GGX 鏡面)。
出力は sRGB へ変換し、トーンマッピングはしない。ライト強度は物理ベースの慣習
(拡散 = albedo / π × 放射照度) に従う。

host (上位) が持つもの: canvas、カメラ行列、フレームループ、入力の意味付け。
Pictor が持つもの: WebGL2 context、shader program、自分が作ったメッシュの GPU 資源。

## 構成

| 層 | ファイル | 責務 |
|---|---|---|
| 入力契約 (純 CPU) | `include/pictor/webgl/web_mesh_input.h`, `src/webgl/web_mesh_input.cpp` | メッシュ・ライト・材質の検証、頂点の詰め替え (位置/法線/線形色の 9 float)、sRGB→線形 |
| 描画 (WebGL2) | `include/pictor/webgl/web_mesh_renderer.h`, `src/webgl/web_mesh_renderer.cpp`, `src/webgl/web_mesh_shaders.h` | context・program・メッシュ資源の所有、begin_frame → submit → end_frame |
| C ABI (Emscripten) | `src/webgl/web_mesh_exports.cpp` | JS からの入口。失敗は 0 を返し `pictor_web_last_error()` に理由 |
| JS の入口 | `web-mesh/` (`@ludiars/pictor-web-mesh`) | `createWebMeshRenderer(canvas)`、型付き配列の wasm heap への受け渡し、ライト値の詰め方 |
| ビルド | `cmake/PictorWebGL.cmake` | `pictor_webgl` に追加、`pictor_web_mesh` (ES module + wasm) を `<build>/web-mesh/` へ |

既存の `WebGLContext` / `WebGLShaderManager` / `WebGLBufferManager` を使い、icosphere の
リファレンス `WebGLRenderer` とは並置する (置き換えない)。

## JS の使い方

```js
const renderer = await createWebMeshRenderer(canvas);            // canvas は document に接続済み
renderer.setLighting({ sky, ground, hemisphereIntensity, directional: [{ direction, color, intensity }] });
renderer.setSurface({ roughness, metalness });
const mesh = renderer.createMesh({ positions, normals, colors, indices }); // 色は既定で sRGB
renderer.beginFrame({ width, height, viewProjection, cameraPosition, clearColor });
renderer.submit({ mesh, transform });                            // column-major 4x4
renderer.endFrame();
renderer.releaseMesh(mesh);
renderer.dispose();
```

- canvas は CSS セレクタで探すので id が要る。id が無ければラッパが付ける。
- width / height は描画バッファの画素数。前フレームと違えば描画バッファの寸法を合わせ直す。
- 色・クリア色・ライト色は sRGB 0..1。`colorSpace: "linear"` で線形の頂点色も受ける。

## 不変条件

- 検証を通らない入力は GPU へ上げない (NaN/Inf、範囲外の添字、3 の倍数でない添字数、0..1 外の色、
  ゼロ長の光の向き、上限超えのライト数、範囲外の材質値)。理由は名前付きのエラーで返す。
- 呼び出し順 (begin_frame → submit* → end_frame) を外れた呼び出しは明示エラー。
- 他のレンダラのメッシュ、解放済みメッシュは拒否する。
- 資源の解放: release_mesh は VAO・頂点バッファ・添字バッファを返す。生成途中で失敗した資源は
  その場で返す。dispose (shutdown) は残る全メッシュ・program・context を返す。
- wasm heap への一時領域は呼び出しごとに確保し、成功・失敗のどちらでも解放する。
- 法線は model 行列の上 3x3 で回す (一様スケールを前提とする)。

## ビルド

Emscripten toolchain では、本体 `pictor` (Vulkan 必須) と GLFW の探索・本体・demo・test を行わず、
`cmake/PictorWebGL.cmake` の web targets だけを構成する。

```sh
emcmake cmake -S . -B build/web -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build/web --target pictor_web_mesh
# → build/web/web-mesh/ に pictor_web_mesh.mjs / .wasm と web-mesh/ の JS を配置
```

consumer は `build/web/web-mesh/` を `npm pack` したものか、そのファイル群を取り込み元の
Pictor commit と一緒に取り込む。兄弟 checkout を実行時に参照しない。

## 検証

- 入力契約: `tests/unit_web_mesh_input_test.cpp` (ネイティブ、CTest)。
- JS の入口: `web-mesh/test/*.test.mjs` (Node、偽の wasm module。CTest の `web_mesh_js_test`)。
- wasm のビルド: emsdk 6.0.9 で `pictor_web_mesh` と既存 `pictor_webgl_demo` が通ることを確認。
- ブラウザでの描画は Pictor 側では未検証 (実行は consumer 側の確認に委ねる)。

## 対象外

テクスチャ、スキニング、影、透過、ポストプロセス、複数材質、インスタンシング、レイキャスト。
必要になった consumer の要求に合わせて足す。
