# consumer への組み込み

Pictor を上位アプリ (KuzuSurvivors 等) に組み込むための CMake / インクルード / define 設定。
オプションの意味は [`build-options.md`](build-options.md)、ビルド手順は [`build.md`](build.md)。

> **レイヤ方向の原則**: Pictor は **最下層**。consumer は Pictor の抽象 API に依存し、Vulkan handle / GLFW を直叩きしない ([[feedback_pictor_no_upper_dep]] / `../../CLAUDE.md` SOLID D)。逆向き (Ergo / AdventureCube / ergo_custos 等の上位を Pictor に取り込む) は禁止 — 本書も「Pictor を下に敷く」方向のみを扱う。

## 1. CMake で取り込む

最小形 (`../../README.md` の「プロジェクトへの組み込み」と同じ):

```cmake
add_subdirectory(Pictor)
target_link_libraries(your_app PRIVATE pictor)
```

`pictor` は include ディレクトリを PUBLIC で公開する (`../../CMakeLists.txt:214-217`) ので、リンクすれば自動でヘッダパスが通る。

### sibling clone を取り込む実例 (KuzuSurvivors)

別ディレクトリにある Pictor を `add_subdirectory` で取り込み、オプションを consumer 側で固定する実パターン (`../../../KuzuSurvivors/CMakeLists.txt`):

```cmake
set(KUZU_PICTOR_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../Pictor" CACHE PATH "Path to Pictor source")

# consumer 側で Pictor のオプションを固定 (FORCE で上書き)
set(PICTOR_BUILD_DEMO      OFF CACHE BOOL "" FORCE)
set(PICTOR_ENABLE_PROFILER ON  CACHE BOOL "" FORCE)
set(PICTOR_ENABLE_RIVE     ON  CACHE BOOL "Rive Renderer 統合")
set(PICTOR_RIVE_DIR        "${CMAKE_CURRENT_SOURCE_DIR}/../rive-runtime" CACHE PATH "rive-runtime のクローンパス")

add_subdirectory(${KUZU_PICTOR_DIR} ${CMAKE_BINARY_DIR}/_pictor)
target_link_libraries(your_app PRIVATE pictor)
```

ポイント:
- consumer の `CMakeLists.txt` で `set(PICTOR_* ... CACHE BOOL "" FORCE)` してから `add_subdirectory` すると、Pictor の `option()` 既定を上書きできる。
- ビルドツリーを分けるため `add_subdirectory(<src> <bin>)` の第 2 引数 (例 `${CMAKE_BINARY_DIR}/_pictor`) を渡す。
- CI など rive-runtime が無い環境では `-DPICTOR_ENABLE_RIVE=OFF` でビルドできるようにしておく。

## 2. インクルード

```cpp
#include <pictor/pictor.h>          // 集約ヘッダ
```

個別ヘッダも `include/pictor/<module>/` 配下で利用できる (例 `#include <pictor/shader/shader_registry.h>`、`#include <pictor/visus/visus.h>`)。

## 3. 必要な define / リンク設定

`pictor` の **PUBLIC** define は consumer に自動伝播する (`../../CMakeLists.txt`)。consumer 側で改めて立てる必要は基本無いが、Vulkan を使う consumer 自身のソースが Vulkan プラットフォームマクロを必要とする場合は併せて指定する。KS の実例では consumer ターゲットにも `PICTOR_HAS_VULKAN` を付けている (`../../../KuzuSurvivors/CMakeLists.txt:288`)。

| 項目 | 必要なとき | 対応 |
|---|---|---|
| `PICTOR_HAS_VULKAN` | consumer 自身が Vulkan API を直接触る | `pictor` から PUBLIC 伝播 (`../../CMakeLists.txt:220`)。consumer ソース側で必要なら明示付与 |
| Rive 統合 | ベクター/Rive を使う | `PICTOR_ENABLE_RIVE=ON` + `PICTOR_RIVE_DIR` (§1 / [`build-options.md`](build-options.md) §Rive)。`Rive::Rive` の `RIVE_VULKAN;WITH_RIVE_TEXT;WITH_RIVE_LAYOUT` define は `pictor` 経由で伝播 (`../../cmake/FindRive.cmake:191`) |
| **MSVC `/utf-8`** | MSVC で Pictor の日本語コメント入り header を取り込む | **consumer ターゲットに `/utf-8` を個別付与**。`pictor` の `/utf-8` は PRIVATE で伝播しない ([[feedback_msvc_utf8_test_targets]]) |

MSVC consumer の例:

```cmake
if(MSVC)
    target_compile_options(your_app PRIVATE /utf-8)
endif()
```

## 4. Release 必須 (Rive 有効時)

`PICTOR_ENABLE_RIVE=ON` の consumer は **Release でビルド・リンクする**。prebuilt `rive_yoga.lib` 等が Release 専用 CRT のため、Debug リンクは LNK2038 / LNK1319 になる ([[feedback_ks_release_build_required]])。コンパイルは通りリンク段で初めて落ちる点に注意。詳細は [`build.md`](build.md) §3。

## 5. シェーダ / アセットの扱い

Pictor のデモシェーダや postprocess シェーダを consumer が流用する場合、consumer 側で SPIR-V を焼く。KS は `${KUZU_PICTOR_DIR}/demo/shaders/` や `${KUZU_PICTOR_DIR}/shaders/postprocess/` を参照してコンパイルする (`../../../KuzuSurvivors/CMakeLists.txt:334-356`)。`PICTOR_BUILD_DEMO=OFF` で取り込んでいても、ソースツリー上のシェーダファイル自体は参照できる。

## 6. GI 経路への移行 (phase 2 — opt-in)

GI (probe grid 間接光 + CSM 実描画) は **opt-in**。既存ホストは `pbr.frag` のまま無変更で動き続ける。有効化する場合:

### 6.1 per-pixel 間接光 (pbr_gi.frag)

1. `GIGpuExecutor` を初期化する (`gi/gi_gpu_executor.h`):
   ```cpp
   pictor::GIGpuExecutor gi_gpu;
   gi_gpu.initialize(vk, shader_dir, max_objects, probe_config);
   ```
   `shader_dir` に `gi_probe_sample.comp.spv` が必要 (無ければ即 false — 黙って縮退しない)。
2. probe SH を供給する。静的シーンなら `GIBakeSystem` の bake 結果、動的ライトなら `GIProbeField::relight()` → `gi_gpu.upload_probe_sh(field.sh_data(), field.probe_count())`。
3. マテリアルを `pbr_gi.frag.spv` に差し替え、descriptor set 2 へ追加バインドする:

   | set 2 binding | 実体 | 型 |
   |---|---|---|
   | 0 | ShadowUniformData (従来どおり) | UBO |
   | 1 | shadow atlas (従来どおり) | sampler2DArray |
   | **2** | `gi_gpu.params_buffer()` | UBO (64B) |
   | **3** | `gi_gpu.probe_sh_buffer()` | SSBO (readonly) |

   `giIntensity` (probe config) を 0 にすればバインドを保ったまま GI を無効化できる。
4. per-object 消費 (カスタムシェーダ / CPU 側 tinting) は `update_objects()` + `record()` (シーン描画前) → `object_irradiance_buffer()` を読む。

### 6.2 CSM 実描画 (GIShadowAtlas)

1. `GIShadowAtlas` を初期化する (`gi/gi_shadow_atlas.h`): resolution / cascade 数は `ShadowMapConfig` と一致させる。
2. 毎フレーム `GILightingSystem::execute()` で cascade 行列を更新後、cascade ごとに `render_pass()` + `framebuffer(c)` で depth pass を begin し、影を落とすメッシュを `cascade_view_proj(c)` で描く (`shaders/shadow_depth.vert/.frag` 提供 — pipeline はホストの頂点レイアウトで組む)。
3. `atlas_view()` + `compare_sampler()` を set 2 binding 1 に、`shadow_uniforms()` を binding 0 に結線する。

### 6.3 TAA / auto exposure のホスト契約 (postprocess)

- TAA: 投影行列へ `pictor::taa_jitter()` のジッタを毎フレーム適用し、`PostProcessConfig::taa` の `reproj_matrix` (ジッタ無し prevVP·inv(currVP)) / `jitter_x/y` を更新する。カメラカット・resize・rebuild_chain 後は `history_valid = false`。
- SSR: `PostProcessConfig::ssr` の `proj_xx / proj_yy / near_plane / far_plane` をカメラと一致させる。
- auto exposure: `HDRConfig::auto_exposure = true` のみ。適応 dt は `record()` が自動供給する。
