# 設計書: 描画方式の切り替え口 (RHI) — Vulkan / DirectX 12 / Metal / WebGPU / WebGL2 (Fable 案)

- 日付: 2026-10-14
- 対象: Pictor (Pc)
- 依頼: `spec/tasks/2026-10-14-multi-rhi-design-request.md` (コミット 7dcffea)
- Actio: `a38aab5b-3141-4093-a108-a71722bd25b0` (Ludellus-Native の Native 一本化 段階 1)
- 種別: plan (設計書)。実装は含まない。もう一方 (Astra) の案は参照していない。
- 根拠の確認基準: 作業場所 `Pictor-design-multi-rhi-fable` のローカル main `70733c5`。
  ブランチ `feat/native-metal-dx12-recovery` は `847b56c`。実機・ビルドは行っていない。

---

## 0. 結論

1. **切り口は 2 層にする。** 消費側 (ゲーム・Ergo・非ゲーム host) が触る「消費面」と、
   Pictor の内側だけが触る「backend 面」を分け、両者の間に `pictor::rhi` 抽象を置く。
   消費面には GPU API の型を 1 つも出さない (`include/pictor/` で `Vk*` / `ID3D12*` / `MTL*` /
   `WGPU*` / `GL*` が 0 件であることを CI が機械検査する)。
2. **hot path は「パス単位で仮想、描画は配列で渡す」。** `CompiledPass` の Vk ハンドル直値は
   backend 表への `uint32_t` index に置き換え、`ICommandEncoder::draw_indexed_packets(const DrawPacket*, n)`
   へ flat 配列を 1 回渡す。per-draw の仮想呼び出しは入れない。string / map ゼロの不変条件は維持する。
3. **5 方式は同じ口に並べ、実行時に選ぶ。** ビルドに含める backend は CMake で選び、起動時は
   「明示指定 > 環境変数 > プラットフォーム既定順」で試行し、失敗理由を `BackendReport` に残す。
   既定順: Windows = Vulkan → D3D12、Linux = Vulkan、macOS = Metal (実装まで Vulkan/MoltenVK)、
   Android = Vulkan、iOS = Metal、Web = WebGPU → WebGL2。
4. **能力差は Pictor が解決し、降格は preset/Visus の宣言に従う。** 消費側コードに `if (backend == …)`
   を書かせない。降格した事実は起動ログ 1 回・StatsOverlay・frame tap の header・`BackendReport`
   の 4 か所に必ず出す (黙って劣化させない)。
5. **シェーダは GLSL (Vulkan 方言) を正本に保ち、ビルド時に 5 方式へ変換して生成物をコミットする。**
   SPIR-V → (SPIRV-Cross) HLSL/MSL/GLSL ES、SPIR-V → (Tint または Naga) WGSL。消費側のカスタム
   シェーダも同じ `pictor_shaderc` を通し、Visus shader package に backend 別 variant と
   reflection を同梱する。Slang への正本切替は未決事項として分けて書く (§8)。
6. **`feat/native-metal-dx12-recovery` は取り込まない。** DX12 / Metal の surface・present・復旧実装は
   既に main に入っており (`dx12_context.h` / `metal_context.h` は main と同一)、ブランチ側にしか無い
   実装は確認できなかった。main の `Dx12Context` / `MetalContext` を backend の種にする。
7. **段階は 10 本。** 親の順序 (Vulkan 移設 → WebGPU → WebGL2 → DX12/Metal) に同意しつつ、
   WebGPU の前に「シェーダ変換基盤」を挟む (WGSL が無いと WebGPU は 1 枚も描けないため)。
   各段階は単独で main に入り、既存 Vulkan 経路は退行しない。

---

## 1. 解釈 (親の解釈への同意・異論)

| 親の解釈 | 判断 | 根拠 |
|---|---|---|
| RHI を作り Vulkan を内側へ移し、5 方式を並べる | 同意 | `include/pictor/` の 42 ヘッダに Vk 型が露出 (§3.1)。並置では消費側が方式を知る |
| 「GL」= WebGL2 (GLES 3.0 相当)、デスクトップ OpenGL は論点 | 同意。**デスクトップ OpenGL は作らない** (推奨) | Vulkan が Win/Linux/Android を覆い、macOS は Metal。GLES3 backend は ANGLE 経由でデスクトップ検証に使えるので、OpenGL 専用実装の価値が無い |
| 消費側コードは方式を一切知らない (最上位の受入条件) | 同意。機械検査を §6 P0 と各段階の受入に入れる | — |
| 既存 DX12 設計の段階計画を 5 方式に広げて作り直す | 同意。ただし「HLSL 正本」は採らない (§4.4) | SHaRC の HLSL 複製 8 本が既に二重管理。GLSL 資産 ≈ 100 本 |
| recovery ブランチを取り込むか・部分移植か | **異論: どちらも不要** | §4.8。547 ファイル差は 3-dot 比較で、2-dot では main の方が +16k 行進んでいる |
| 優先順位 Vulkan → WebGPU → WebGL2 → DX12/Metal | 概ね同意。シェーダ変換基盤を WebGPU の前に置く。D3D12 は Windows SDK だけで済むので WebGL2 と並行可 | §6 |
| `portability/web.md` 決定 2「WebGL2 は RHI の対象にしない」 | **異論: 対象にする** | neco 2026-10-14 の「WebGPU 主、WebGL2 予備」。縮小プロファイル backend として同じ口に入れる |

補足: 「それぞれのプラットフォームでなんも考えなくていい」を本書では次の 3 条件に分解し、
それぞれに検査を付ける (§6.1)。
(a) 公開ヘッダに方式固有の型が無い、(b) 消費側の同一ソースが backend 構成だけ変えて全方式で
コンパイル・リンクできる、(c) 消費側が方式分岐 (`#ifdef PICTOR_HAS_VULKAN` 等) を書かなくて済む
(= Pictor の backend マクロを consumer へ伝播させない)。

---

## 2. 現状の事実 (設計の入力)

- 公開ヘッダ 42 本が `Vk*` を含む。中核は `surface/vulkan_context.h`、`pipeline/compiled_graph.h`
  (`CompiledPass` が `VkRenderPass` / `VkFramebuffer[4]` / `VkDescriptorSet[4]` / `VkPipeline` /
  `VkClearValue[8]` 直値)、`pipeline/render_pass_scheduler.h` (`PassRecordFn(VkCommandBuffer, …)`)、
  `core/pictor_renderer.h` (`compile_render_graph(VkDevice, …)` / `render_compiled(VkCommandBuffer, …)`)、
  `profiler/gpu_timer.h` (`VkQueryPool`)、`pipeline/compiled_batch_recorder.h` (`BatchGpuResources` が
  `VkPipeline` / `VkBuffer`)、GI / SHaRC / decal / postprocess / UI / XR / Rive の各 executor。
- `src/` で Vk を叩く cpp は 33 本 (pipeline 7、gi 6、profiler 4、surface 3、ui 2、xr 3、他)。
- `PICTOR_HAS_VULKAN` / `PICTOR_HAS_DX12` / `PICTOR_HAS_METAL` / `PICTOR_HAS_WEBGL` は
  **PUBLIC** compile definition として consumer へ伝播している (`CMakeLists.txt:271,280,299`、
  `cmake/PictorWebGL.cmake`)。Ergo の `ergo_render` はこれを「実描画可否の正本」にしている
  (`Ergo/include/ergo/render/render_backend.h`)。
- `ISurfaceProvider::get_required_instance_extensions()` が Vulkan 専用の問い合わせ。
  `NativeWindowHandle` 自体は OS ハンドルのみで方式非依存。
- `FrameResult` / `FrameStatus` / `ContextInitResult` / `gate_frame()` は方式非依存で、
  `VulkanContext` / `Dx12Context` / `MetalContext` の 3 つが同じ語彙を返す (`mobile-surface-recovery.md`)。
  `PerImageResourcePool<Backend>` は backend policy テンプレートで、fake を渡した単体テストがある。
- `Dx12Context` / `MetalContext` (main) は swapchain + clear + `NativeMeshView` の upload/draw +
  `recover_surface` / `recover_device` を持つ。描画は `NativeDrawCallback(void* encoder, …)` で
  生エンコーダを host へ渡す形 (= 消費側が方式を知る形。置き換え対象)。
- `pictor_webgl` は別 static lib。`WebGLContext` / `WebGLShaderManager` / `WebGLBufferManager` /
  `WebGLRenderer` (icosphere 1 種) / `WebMeshRenderer` (配布済み ES module `pictor_web_mesh`)。
  Emscripten toolchain では root CMake が web target だけを構成して `return()` する。
  本体 `pictor` は `pictor.h` が `vulkan_context.h` を無条件 include するため wasm 不可。
- シェーダは GLSL (Vulkan 方言) → glslc で SPIR-V。`shaders/` 直下 33 + `postprocess` 27 +
  `sharc` 18 (+ HLSL 複製 8) + `ui` 4 + `decal` 1 + `xr` 2 + `webgl` 4 (GLSL ES 3.00、別実装)。
  `VisusShaderPackage` は `VisusShaderRef` (builtin: / stages / visus:) と params / metadata を持ち、
  stages は SPIR-V パスを指す (`shader_registry.h` の `CustomShaderDef{vert_spv, frag_spv}`)。
- 消費側: Ludellus-Native の `world_renderer.cpp` は `pictor::VulkanContext&` と `VkCommandBuffer` /
  `VkRenderPass` / `VkFramebuffer` を直接扱う (35 + 21 + 13 箇所)。Ergo の `IRenderLayer::record(VkCommandBuffer, VkExtent2D)` /
  `FrameComposer::add_pass(VkRenderPass, …)` / `vk_fwd.h` が公開 API で Vk 型を受ける。
  Ergo の `ergo_gpu_particle::IGpuBackend` は buffer / 1D texture / compute dispatch だけの薄い
  interface で Vulkan / WebGPU 両対応の前例。
- CI は Ubuntu GCC 1 ジョブ (Vulkan + glslang、`pictor` ライブラリのビルドのみ)。MSVC・Emscripten・
  macOS のジョブは無い。

---

## 3. 比較した案

### 3.1 RHI の粒度と選択方式 (設計項目 1)

| 案 | 内容 | 利点 | 欠点 |
|---|---|---|---|
| A. 全面仮想 (UE RHI 型) | Device / Encoder / Resource を全部 virtual。per-draw も virtual | 実装が素直。backend を実行時に選べる | per-draw 仮想呼び出しが hot path に入る。DoD 方針 (`portability.md` §2.1「仮想呼び出しを per-draw に入れない」) と衝突 |
| B. コンパイル時静的選択 (`using Backend = VulkanBackend`) | 1 ビルド 1 backend。テンプレート / `if constexpr` | 仮想ゼロ、最速 | Web の「WebGPU 無ければ WebGL2」を 1 バイナリで実現できない。backend ごとに全テンプレートの instantiation が増えビルド時間が伸びる |
| **C. パス単位仮想 + 描画は配列 (推奨)** | 生成・swapchain・pass の begin/end は virtual。描画は `DrawPacket` の flat 配列を 1 回で渡し backend 内でループ | 仮想呼び出しはパスあたり数回 (≤ 32 pass で ≈ 100 回/frame)。backend の実行時選択が可能。CompiledPass の不変条件を保てる | `DrawPacket` の形を 5 方式の最大公約数で固定する必要がある。backend ごとにループ実装が要る (ただし今の `CompiledBatchRecorder::record_batches_` と同じ形) |

ハンドルは `struct BufferHandle { uint32_t index; }` 型の index (backend 側の flat 表を引く)。Debug では
上位 8 bit に generation を入れて誤用を検出する。`CompiledPass` は ≤ 256 B のまま。

### 3.2 シェーダ正本と変換経路 (設計項目 4)

| 案 | 経路 | 利点 | 欠点 |
|---|---|---|---|
| **S1. GLSL 正本 + 変換 (推奨)** | GLSL → glslang → SPIR-V → SPIRV-Cross (HLSL→DXC→DXIL / MSL / GLSL ES 3.00)、SPIR-V → Tint/Naga → WGSL | 既存 ≈ 100 本を書き直さない。glslang は CI に既にある。Godot 4 の D3D12 経路と同じ構成で前例が多い | DXIL / WGSL が 2 段変換。SPIR-V に入った後は GLSL の `#include` やマクロ情報が消えるため、エラー位置は変換ツールの診断に頼る |
| S2. HLSL 正本 (既存 DX12 設計) | HLSL → DXC → SPIR-V と DXIL、SPIR-V → SPIRV-Cross / Tint | DXIL が直。SHaRC の HLSL 8 本がそのまま使える | GLSL ≈ 100 本の一括移植。Metal / WebGPU / WebGL2 は結局 SPIR-V 経由の変換になり、S1 と変わらない |
| S3. Slang 正本 | Slang → SPIR-V / DXIL / MSL / WGSL / GLSL を 1 コンパイラで出力 | 1 ツールで 5 方式。reflection 付き。GLSL 互換フロントエンドで段階移行可 | slangc をツールチェーンに加える (SDK 取り込み禁止方針と調整要)。WGSL 出力は新しく、ブラウザ実機での検証履歴が浅い。neco の判断事項 (§8) |
| S4. WGSL 正本 | WGSL → Naga/Tint → SPIR-V / MSL / HLSL / GLSL | Web 主目的なら直 | 既存資産ゼロ流用。compute の拡張機能 (subgroup 等) が制限される |

### 3.3 Web の実装形態 (設計項目 7)

| 案 | 内容 | 利点 | 欠点 |
|---|---|---|---|
| W1. Emscripten のみ (emdawnwebgpu) | WebGPU backend は `webgpu.h` 準拠で書き、Emscripten の `--use-port=emdawnwebgpu` でリンク | 依存が emsdk だけ | WebGPU backend の検証がブラウザ上だけになる。CI で実行確認できない |
| **W2. `webgpu.h` 準拠 + ネイティブは opt-in (推奨)** | 同じソースを Emscripten では emdawnwebgpu、デスクトップでは `PICTOR_WEBGPU_NATIVE_DIR` (Dawn または wgpu-native の prebuilt) を指したときだけリンク | デスクトップで WebGPU backend を動かして単体で検証できる。fetch しない (既存方針) | prebuilt の配置は人手。`webgpu.h` の版差 (Dawn / wgpu-native) を 1 本に揃える必要 |
| W3. 既存 `pictor_webgl` を別実装のまま温存 | `WebGLRenderer` を RHI 外に残す | 作業ゼロ | 消費側が「Web のときは別 API」を知ることになる = 受入条件違反 |

WebGL2 は **GLES3 backend** として RHI に入れる (`src/rhi/gles3/`)。`pictor_webgl` の context / shader /
buffer 管理は backend の内部実装へ移し、`WebGLRenderer` (デモ用 icosphere) は廃止、`WebMeshRenderer`
(配布済み `pictor_web_mesh`) は GLES3 backend の上に載せ替えて API を維持する。

### 3.4 recovery ブランチの扱い (設計項目 8)

| 案 | 内容 | 判断 |
|---|---|---|
| R1. 全体マージ | 547 ファイル (3-dot) を取り込む | 不可。2-dot 比較ではブランチの方が 16,156 行少なく、main の frame tap / XR / Visus v2 / 復旧契約の一部 (`context_init_result.h` / `frame_gate.h` / `vulkan_capability_check.h`) を**消す**方向のマージになる |
| R2. 必要部分の移植 | surface 実装だけ cherry-pick | 不要。`include/pictor/surface/dx12_context.h` / `metal_context.h` / `src/surface/dx12_context.cpp` / `metal_context.mm` は main と同一 (`git diff main feat/… -- …` が空) |
| **R3. 取り込まない (推奨)** | ブランチは履歴として凍結。main の `Dx12Context` / `MetalContext` を backend の種にする | `git cherry` が「main に無い」と出す 19 commit は 2026-09-16 の履歴再作成 (非公開アーカイブ) による patch-id 不一致で、内容は main に再作成済み |

### 3.5 公開 API 移行 (設計項目 9)

| 案 | 内容 | 判断 |
|---|---|---|
| M1. 一括置換 | Vk 型を出す API を 1 PR で全部消す | Ergo / Ludellus-Native が同時に壊れる。段階ごとに main へ入れられない |
| **M2. 互換期間つき (推奨)** | 消費面を先に用意し、Vk 型を出す旧 API は `include/pictor/rhi/vulkan/` (interop 面) へ移して `[[deprecated]]`。2 マイナー版の間残す | 消費側が段階 (Ln 段階 4、Ergo 側の改修) に合わせて移れる。CI の消費面検査は interop 面を除外し、interop 面の include 数を別途報告する |

### 3.6 render pass の表現 (設計項目 5)

| 案 | 内容 | 判断 |
|---|---|---|
| P1. Vulkan の explicit render pass を共通モデルにする | `VkRenderPass` 相当の object を全 backend で用意 | D3D12 / Metal / WebGPU は object を持たないので空実装になる。subpass / input attachment は WebGPU / WebGL2 に無い |
| **P2. 「begin 時に attachment 列を渡す」(dynamic rendering 型、推奨)** | `RenderTargetDesc` を compile 時に解決し、backend は begin で受ける。Vulkan backend は compile 時に `VkRenderPass` + `VkFramebuffer` を作って index で持つ (1.3 の dynamic rendering があれば使う) | 4 方式の自然な形。既存 `AttachmentOpsDef` (load/store/layout) はそのまま入力になる |

---

## 4. 推奨案の詳細

### 4.1 RHI の境界 (設計項目 1)

#### 層と置き場所

```
include/pictor/rhi/            消費面と拡張面の公開抽象 (方式固有の型ゼロ)
  backend_kind.h               enum class BackendKind { Auto, Vulkan, D3D12, Metal, WebGPU, WebGL2 }
  capabilities.h               struct Capabilities / FeatureBits / Limits / ClipSpace
  backend_report.h             struct BackendReport (選択結果・試行・降格の記録)
  handles.h                    BufferHandle / TextureHandle / SamplerHandle / PipelineHandle /
                               BindGroupHandle / RenderTargetHandle / QueryHeapHandle (全部 uint32 index)
  types.h                      Format / Extent2D / Rect2D / ClearValue / ResourceState / BufferUsage / …
  device.h                     class IDevice
  swapchain.h                  class ISwapchain  (FrameResult を返す)
  command_encoder.h            class ICommandEncoder / struct DrawPacket
  query_heap.h                 class IQueryHeap (timestamp)
  shader_module.h              struct ShaderBlob (backend 別 variant の選択結果)
  context.h                    class Context (device + swapchain + per-frame encoder の所有者。factory)
  custom_pass.h                class IRecordedPass (拡張面: host が encoder でコマンドを記録する口)
include/pictor/rhi/vulkan/     interop 面 (opt-in。XR / Rive のように Vk を要する host だけが include)
  vulkan_context.h             現 VulkanContext (backend の実体。消費面検査の対象外、include 数を報告)
src/rhi/common/                selection / report / capability table / downgrade resolver / handle table
src/rhi/vulkan/  src/rhi/d3d12/  src/rhi/metal/  src/rhi/webgpu/  src/rhi/gles3/   backend 実装 (PRIVATE)
```

- 消費面 (Tier 0): `PictorRenderer` / `c_api.h` / `ObjectDescriptor` / Visus / profile / `ISurfaceProvider` /
  `FrameResult` / `BackendKind` / `Capabilities` / `BackendReport`。**GPU の操作は一切無い**。
  ゲームと Ludellus-Native の `world/*` `app/*` はここだけを使う。
- 拡張面 (Tier 1): `IRecordedPass` + `ICommandEncoder` + `IDevice`。方式非依存だが GPU 操作を伴う。
  Ergo の `ergo_render` (`IRenderLayer`) や post-process を自前で組む host が使う。
- interop 面 (Tier 2): `include/pictor/rhi/<backend>/`。方式固有。XR runtime (VkInstance 共有)、
  Rive (VkDevice 直接)、既存 consumer の移行期間用。consumer が include したら「方式を知った」と
  みなし、消費面検査の報告に件数が出る。

#### どこで切るか

| 対象 | 切り方 | 備考 |
|---|---|---|
| デバイス | `IDevice` (生成・能力照会・idle 待ち・`DeviceMemoryProfile`) | UMA 判定は既存 `analyze_device_memory()` の入力語彙を各 backend が埋める |
| スワップチェーン | `ISwapchain::acquire() / present() → FrameResult`、`resize()`、`extent() / format() / image_count()` | `gate_frame()` と sticky な loss を backend 共通部 (`src/rhi/common/frame_gate_runner`) に置き、backend は native 呼び出しだけ |
| コマンド記録 | `ICommandEncoder` (フレームごとに Context が貸す)。begin/end render pass・set_pipeline・set_bind_group(space, handle, dynamic offsets)・set_vertex/index buffer・viewport/scissor・`draw_indexed_packets`・`draw_indirect`・`dispatch` / `dispatch_indirect`・`barrier(const Transition*, n)`・`write_timestamp`・copy・`push_data(≤128 B)`・debug marker | `push_data` は WebGPU / WebGL2 では UBO ring に落とす (backend 内) |
| リソース | `create_buffer / create_texture / create_sampler` と `upload` (staging の要否は backend 内。UMA なら直接 map) | `GPUBufferManager` の論理割当 (`GpuAllocation`) はそのまま、実体 handle を backend 表に持つ |
| パイプライン | `create_graphics_pipeline(GraphicsPipelineDesc)` / `create_compute_pipeline` | `GraphicsPipelineDesc` は `VertexLayout` (既存、Vk 非依存) + `RenderTargetFormats` + blend/depth/raster の値型 |
| クエリ | `IQueryHeap` (timestamp のみ。occlusion は非目標) | `GpuTimerManager::initialize_vulkan(VulkanContext&)` → `initialize(IDevice&)` |
| バインディング | `BindGroupLayoutDesc` / `BindGroupDesc` (§4.3) | — |

#### 仮想か静的か・hot path

- 生成系・swapchain・pass の begin/end・配列描画・dispatch・barrier は virtual。**1 パスあたりの仮想呼び出しは
  begin / set_bind_group(PASS) / draw_indexed_packets / end の 4 回程度**。
- `DrawPacket` (64 B、`alignas(64)` 配列):
  `PipelineHandle pipeline; BindGroupHandle material; BindGroupHandle draw; BufferHandle vb, ib;
   uint32_t ib_offset, index_count, instance_count, first_index, first_instance; int32_t vertex_offset;
   uint8_t index_type; uint8_t flags;` — `CompiledBatchRecorder` が `RenderBatch` 列から毎フレーム組む
  (今の `record_batches_` と同じ走査)。backend は packet 配列をループして native draw を発行する。
- `CompiledPass` の Vk 直値は `RenderTargetHandle render_target; std::array<BindGroupHandle,4> pass_inputs;
  PipelineHandle pipeline_override; Rect2D render_area; std::array<ClearValue,8> clears;` に置換。
  `ClearValue` は 16 B の値型。`sizeof(CompiledPass) ≤ 256` の static_assert を維持。
- backend 表 (`std::vector<VkBuffer>` 等) は init / compile 時にだけ伸びる。hot path は index 引きのみ。
- backend の実行時選択は `Context::create()` の中だけで起こり、以後 `IDevice*` / `ISwapchain*` は固定。

#### 消費側が方式を知らずに済む公開面

公開するもの: `BackendKind` (読むだけ)、`Capabilities` (読むだけ)、`BackendReport` (読むだけ)、
`RendererConfig::backend` (希望。`Auto` が既定)、`ContextInitResult` (失敗理由)、`FrameResult`。
閉じるもの: 全ハンドルの実体、native encoder、拡張名、descriptor / root signature / argument buffer /
bind group の実体、シェーダのバイナリ形式、swapchain の image 本体 (readback は `IDevice::readback_texture`
経由)。

### 4.2 5 方式の能力差と降格規則 (設計項目 2)

#### 能力表 (設計上の前提。実機確認は各段階の受入で行う)

| 能力 | Vulkan | D3D12 | Metal | WebGPU | WebGL2 |
|---|---|---|---|---|---|
| compute | ○ | ○ | ○ | ○ | **×** |
| storage buffer (SSBO) | ○ | ○ (UAV) | ○ | ○ | **×** (UBO ≤ 64 KB 典型、最小 16 KB) |
| indirect draw / dispatch | ○ | ○ | ○ | ○ | × |
| indirect count (GPU-driven) | △ (1.2 / `KHR_draw_indirect_count`) | ○ (ExecuteIndirect) | △ (ICB) | × | × |
| timestamp query | ○ | ○ | △ (sample buffer、機種依存) | △ (`timestamp-query` feature。ブラウザが量子化) | △ (`EXT_disjoint_timer_query_webgl2`、多くの環境で無効) |
| bindless | △ (descriptor indexing) | ○ (SM 6.6) | ○ (argument buffer tier 2) | × | × |
| MSAA | ○ | ○ | ○ | ○ (4x のみ) | ○ (renderbuffer) |
| push constants (≤ 128 B) | ○ | ○ (root constants) | ○ (`setBytes`) | × (UBO で代替) | × (uniform で代替) |
| render pass | 明示 (または dynamic rendering) | 暗黙 (RenderPass API は任意) | 明示 (encoder ごとの descriptor) | 明示 (descriptor) | 暗黙 (FBO bind) |
| subpass / input attachment | ○ | × | △ (tile memory、独自) | × | × |
| geometry / tessellation | ○ | ○ | × (mesh shader) | × | × |
| clip space | z 0..1、y 下 | z 0..1、y 上 | z 0..1、y 上 | z 0..1、y 上 | **z −1..1**、y 上 |
| 深度形式 | D24S8 / D32 / D32S8 | 同左 | D32S8 のみ (D24S8 は機種依存) | depth24plus / depth32float | DEPTH24_STENCIL8 / DEPTH_COMPONENT32F |
| 初期化 | 同期 | 同期 | 同期 | **非同期** (adapter / device 要求) | 同期 |

#### 能力照会の型

```cpp
struct Capabilities {
    BackendKind kind;
    uint32_t    feature_bits;        // Feature::Compute | StorageBuffer | IndirectDraw | IndirectCount |
                                     // Timestamp | Bindless | PushConstants | Subpass | Tessellation | …
    Limits      limits;              // max_bind_groups, max_ubo_bytes, max_storage_bytes, max_push_bytes,
                                     // max_texture_units, max_color_attachments, msaa_mask, max_workgroup …
    ClipSpace   clip;                // ZeroToOne / MinusOneToOne, y_up
    uint32_t    depth_formats_mask;
    const char* adapter_name;        // 診断用。hot path で読まない
};
```

consumer は読めるが、読まなくても動く (Pictor が `Camera` の射影補正と深度形式の選択を済ませる)。

#### 降格規則 (消費側に書かせない)

降格の判断は **profile preset / モジュール宣言 / Visus 定義** の 3 か所の宣言から Pictor が決める。
消費側 C++ には分岐を書かない。

| 宣言場所 | 書式 (JSON) | 例 |
|---|---|---|
| render pass (profile) | `"requires": ["compute"], "on_missing": "disable" \| "substitute:<pass_name>" \| "error"` | `gpu_cull` pass: compute 無しなら `substitute:cpu_cull` |
| モジュール manifest (PC-RULE-002) | `"backends": ["vulkan","d3d12","metal","webgpu"], "on_unsupported": "error" \| "disable"` | SHaRC: WebGL2 では `error` (黙って抜かない) |
| Visus / shader package | `"variants": [{"requires":["compute"], "shader": "avatar_sdf"}, {"shader": "avatar_mesh"}]` | SDF アバター → 三角形アバター (Ludellus の要件) |
| profiler | 既存の simulation 経路 | timestamp 無しは CPU timer のみ。`BackendReport` に記録 |

規則:
1. 宣言が無い機能が無いときは **error** (初期化失敗、理由つき)。黙って空にしない (framework-boundaries 「空の処理に黙って置き換えない」)。
2. 降格は init / profile 切替 / backend 切替のときだけ解決する (`DowngradeResolver`、`src/rhi/common/`)。
   フレーム内で再判定しない。
3. 結果は `BackendReport::downgrades[] { feature, from, to, reason }` に残し、
   (a) 起動ログに 1 回、(b) `StatsOverlay` に 1 行 (`Backend: WebGL2 (fallback from WebGPU: adapter unavailable) · downgraded: gpu_cull→cpu_cull, avatar_sdf→avatar_mesh`)、
   (c) frame tap の header 行 (render-tap/1 の `backend` / `downgrades` フィールド追加は `spec/feature/frame-tap.md` 側で版を上げる)、
   (d) `PictorRenderer::backend_report()` で読める、の 4 か所へ出す。
4. 降格は常に「宣言済みの代替」へ。Pictor が勝手に別の描画を発明しない。

### 4.3 バインディングモデルの共通化 (設計項目 3)

4 つの **space** を固定する (WebGPU の最大 bind group 数 4 に合わせる)。

| space | 更新頻度 | 内容 |
|---|---|---|
| 0 FRAME | フレーム 1 回 | カメラ / 時刻 / 共通 UBO |
| 1 PASS | パス 1 回 | 前パスの出力 (shadow atlas、GI table、HDR color) のサンプル |
| 2 MATERIAL | バッチごと | テクスチャ + パラメータ UBO (Visus package の params) |
| 3 DRAW | バッチごと | instance SSBO (無ければ instance 頂点属性)、`push_data` 代替 UBO ring |

対応表:

| Pictor | Vulkan | D3D12 | Metal | WebGPU | WebGL2 |
|---|---|---|---|---|---|
| space | descriptor set index | root signature の descriptor table (space ごとに 1 range 集合)、space 0/3 の小 UBO は root CBV | argument buffer `[[buffer(space)]]` (tier 1 で足りる) か SPIRV-Cross の MSL binding remap で直 index | bind group index | 無し。reflection が (space, binding) → uniform block binding point / texture unit に平坦化 |
| binding | binding | register (b/t/u/s) + space | index | binding | block index / unit 番号 |
| UBO | uniform buffer (dynamic offset) | CBV | `constant` | uniform | uniform block (≤ 64 KB) |
| SSBO | storage buffer | SRV/UAV (Structured) | `device` | storage | **無し** → instance 頂点属性 (divisor 1) に落とす。今の `pictor_webgl` の 80 B/instance 方式 |
| push | push constants | root constants | `setBytes` | UBO ring (backend 内) | uniform ring |
| bindless | descriptor indexing | SM 6.6 heap index | argument buffer tier 2 | 非対応 (Feature bit で隠す) | 非対応 |

- Pictor の資源宣言側は `BindGroupLayoutDesc { space; entries[] { binding; kind; stage_mask; count; } }`。
  **手書きしない**。`pictor_shaderc` の reflection (SPIR-V から) が生成し、shader package に同梱する。
  不一致 (profile が要求する space 1 の内容と shader の宣言のずれ) は compile 時 error。
- GLSL 側の規約は `shaders/common.glsl` に `layout(set = SPACE_*, binding = …)` のマクロで固定。
  既存シェーダはこの規約への揃え直しが要る (資産移行量は §4.4)。

### 4.4 シェーダ戦略 (設計項目 4)

- **正本**: GLSL (Vulkan 方言、`#version 450`)。既存資産と CI の glslang をそのまま使う。
- **変換経路** (すべてビルド時。ツール `tools/shaderc/` = `pictor_shaderc`):

```
*.vert/*.frag/*.comp ──glslang──▶ .spv ──SPIRV-Cross──▶ .hlsl ──DXC──▶ .dxil      (D3D12)
                                       ├─SPIRV-Cross──▶ .metal (MSL)  ──[macOS のみ metal/metallib]▶ .metallib (Metal)
                                       ├─SPIRV-Cross──▶ .glsl (ES 3.00)                         (WebGL2)
                                       └─Tint または Naga──▶ .wgsl                              (WebGPU)
                              └─reflection (SPIRV-Reflect または SPIRV-Cross) ──▶ .refl.json
```

- **生成物はコミットする** (`shaders/gen/<backend>/…`)。理由: consumer と CI に DXC / Tint / Naga /
  Metal toolchain を要求しない (SDK を取り込まない・暗黙 fetch しない既存方針)。CI はツールがある
  ジョブ (Windows: DXC は Windows SDK 同梱、macOS: `metal`、Linux: `naga-cli` または Dawn の `tint`
  を手動配置) で再生成して diff が空であることを検査する。ツールが無いジョブは生成物をそのまま使う。
- **実行時コンパイル**: Vulkan の開発時ホットリロード (既存 `pipeline-hot-reload`、glslang 実行時呼出) と、
  WebGPU (WGSL テキストはブラウザがコンパイル) / WebGL2 (GLSL ES テキスト) / Metal (metallib が無い
  ときの MSL 実行時コンパイル) は backend の内部事情で、消費側は `ShaderBlob` を指すだけ。
- **可搬サブセット**: 5 方式で出るものだけを許す。`pictor_shaderc` が違反を error にする。
  geometry / tessellation、subpass input、64-bit atomic、`gl_DrawID`、push constant > 128 B、
  bindless 配列、`shared` > 16 KB、subgroup 命令 (Feature で隠す)。非対応のシェーダは
  モジュール manifest で backend 限定 (例: ocean の tessellation は Vulkan/D3D12 demo 限定)。
- **Visus shader package との関係**: `VisusShaderRef` の stages を「package 名 + stage」にし、
  package 側が `variants[backend][stage] = path` と `refl.json` を持つ。loader は `BackendKind` で
  variant を選ぶ。`ShaderRegistry::CustomShaderDef{vert_spv, frag_spv}` は `ShaderBlobSet` に替える。
  `shaderpkg.json` の書式版を上げ、既存 `visus_migrate` で v2 → v3 変換を用意する。
- **消費側カスタムシェーダ**: 消費側は GLSL 1 ソースを書き、`pictor_shaderc` を自分のビルドで回す
  (CMake 関数 `pictor_compile_shaders(<target> <sources> <out_dir>)` を Pictor が export。
  `cmake/PictorEffectShaders.cmake` の拡張)。出力 package を Visus から参照する。Web 配信は
  package をそのまま asset として fetch。
- **既存 GLSL 資産の移行量** (本書の推定、実測ではない):
  - binding 規約 (space マクロ) への揃え直し: 全 ≈ 100 本。機械的 (sed 級) だが人手確認が要る。
  - 可搬サブセット違反の洗い出し: `ocean.tesc/.tese` (tessellation、demo 限定扱い)、
    `sharc_*` の `shared` 使用量 (13.5 KB、可)、push constant の大きさ、`gl_DrawID` 不使用の確認。
  - `shaders/webgl/*.glsl` (4 本) と `shaders/sharc/hlsl/*` (8 本) は生成物に置き換えて削除。
  - 1 方式で意味が変わる箇所: 深度範囲 (WebGL2 の −1..1 は射影行列で吸収、シェーダ側は変更不要)、
    テクスチャ Y 反転 (SPIRV-Cross の `flip_vert_y` で吸収)。

### 4.5 パイプライン系 (PipelineProfile → CompiledGraph) の RHI 化 (設計項目 5)

- **render pass モデル**: §3.6 P2。`RenderPassDef` + `AttachmentOpsDef` (既存、Vk 非依存) から
  `PipelineCompiler` が `RenderTargetDesc { color[8]{texture, load, store, clear}, depth{…},
  resolve, extent }` を作り、`IDevice::create_render_target(desc, slot_count)` で backend 表に登録する。
  Vulkan backend はここで `VkRenderPass` + `VkFramebuffer` (slot ごと) を作る。他 backend は
  descriptor を保持するだけ。
- **CompiledPass**: §4.1 のとおり handle 化。`flags` の `IS_SWAPCHAIN / IS_COMPUTE / IS_HOST_RECORDED` は維持。
- **registries**: `AttachmentRegistry` / `RenderPassRegistry` / `FramebufferRegistry` の `initialize_vulkan(VkDevice, …)`
  は `initialize(IDevice&, …)` に替える。`FramebufferRegistry` は `RenderTargetRegistry` に改名し
  `(pass_index, slot) → RenderTargetHandle`。`set_external_render_pass(VkRenderPass)` は interop 面へ。
- **レイアウト遷移**: `AttachmentOpsDef::initial_layout / final_layout` (既存 `ImageLayout` 列挙) から
  `PipelineCompiler` がパス間の `Transition[]` を作り、パス index の side table に置く (CompiledPass を
  太らせない)。Vulkan / D3D12 backend は barrier を発行し、Metal / WebGPU / WebGL2 は no-op。
- **input attachment / subpass**: 使わない。前パス出力のサンプルは space 1 (PASS) の bind group。
  既存 `CompiledPass::input_sets[4]` はその置き換え。
- **host_recorded パス**: `PassRecordFn(ICommandEncoder&, const CompiledPass&, flight, image)`。
  `PostProcessPipeline` (61 箇所の Vk) は **モジュールとして RHI 化** (§6 P2b)。それまでは manifest で
  `backends: [vulkan]` を宣言し、他 backend では明示 error。
- **PerImageResourcePool<Backend>**: 既存の policy テンプレートをそのまま各 backend の per-image
  (command buffer / semaphore 相当) に再利用する。fake backend の単体テストも流用。
- **GPU-driven**: `GPUDrivenPipeline::execute` の dispatch 未実装 (現状) は、RHI 化後に
  `ICommandEncoder::dispatch` で実装する。`IndirectCount` が無い backend では profile の
  `on_missing` に従う (既定 preset は `substitute:cpu_cull`)。

### 4.6 ウィンドウ / 表示面と方式の自動選択 (設計項目 6)

- `ISurfaceProvider` は維持。変更点:
  - `NativeWindowHandle::Type` に `WebCanvas { const char* selector; }` と `MetalLayer` (macOS の
    `CAMetalLayer`。`Cocoa` は `NSView*` 用に残す) を追加。
  - `get_required_instance_extensions()` を **interface から外す**。Vulkan backend が `Type` から
    拡張名を引く (Win32 → `VK_KHR_win32_surface` 等。GLFW が返す内容と同じ)。これで provider から
    Vk 語彙が消える。
- backend ごとの swapchain:

| backend | 実体 | present | 復旧 |
|---|---|---|---|
| Vulkan | `VkSwapchainKHR` (既存) | `vkQueuePresentKHR` | 既存 `FrameResult` 経路 |
| D3D12 | `IDXGISwapChain3` flip (main の `Dx12Context` 由来) | `Present` | `DXGI_ERROR_DEVICE_REMOVED` → `DeviceLost` (既存 `recover_device`) |
| Metal | `CAMetalLayer` drawable (main の `MetalContext` 由来) | `presentDrawable` | layer 消失 → `SurfaceLost` |
| WebGPU | `WGPUSurface` + `wgpuSurfaceConfigure` (canvas) | `wgpuSurfacePresent` (Emscripten では rAF 復帰で自動) | `device lost` callback → `DeviceLost`。canvas 縮小 0 → `RecreateSwapchain` |
| WebGL2 | 既定 framebuffer (object 無し) | rAF 復帰 | `webglcontextlost` → `DeviceLost`、`webglcontextrestored` 後に再初期化 |

- `FrameResult` / `FrameStatus` / `ContextInitResult` / `gate_frame()` は変更せず、`ISwapchain` の契約に
  する。sticky loss と `presentation_suspended` の処理は `src/rhi/common/` の共通部へ移し、backend は
  native 呼び出しの結果を `FrameStatus` に写すだけにする (`Dx12Context` / `MetalContext` の既存
  状態機械を共通部へ寄せる)。
- **非同期初期化 (Web)**: WebGPU の adapter / device 要求は非同期。`Context::create()` は Web では
  `Context::begin_create()` + `Context::poll() → CreateState{Pending, Ready, Failed}` の 2 段にし、
  host (Ludellus の app 層) は rAF ループで `Ready` まで回す。ネイティブ backend は `begin_create()` 内で
  同期完了する (同じ呼び出し列で動く = 消費側コードは共通)。
- **自動選択規則**:
  1. 候補列 = `RendererConfig::backend` が `Auto` 以外ならその 1 つ。`Auto` なら環境変数
     `PICTOR_BACKEND` (あれば 1 つ)、無ければプラットフォーム既定順 (§0-3)。
  2. ビルドに含まれない候補は `ContextInitStatus::MissingCapability("backend not built")` として記録し次へ。
  3. 候補ごとに `begin_create()`。失敗は `BackendReport::attempts[] { kind, ContextInitResult }` に残す。
  4. 全滅なら `PictorRenderer::initialize()` は false、`backend_report()` に全試行が残る。
  5. 選ばれた backend が既定順の先頭でないときは `BackendReport::fallback_from` に理由を記録し、§4.2 の 4 か所へ出す。
- `PictorRenderer` が `rhi::Context` を所有する (今は host が `VulkanContext` を所有)。XR のように
  instance を共有する host は `RendererConfig::external_device_requirements` (interop 面の型) で要求を渡す。

### 4.7 WebGPU / WebGL2 の実装形態 (設計項目 7)

- WebGPU backend は `webgpu.h` (webgpu-native 標準ヘッダ) に対して書く。Emscripten では
  `--use-port=emdawnwebgpu`。デスクトップは `PICTOR_WEBGPU_NATIVE_DIR` に Dawn または wgpu-native の
  prebuilt があるときだけ `PICTOR_RHI_WEBGPU=ON` が成立する (fetch しない)。`webgpu.h` の版は
  Dawn の 2025 以降 (`WGPUStringView` 世代) に固定し、`src/rhi/webgpu/webgpu_compat.h` で差を吸収する。
- WebGL2 (GLES3) backend は `src/rhi/gles3/`。`pictor_webgl` の `WebGLContext` / `WebGLShaderManager` /
  `WebGLBufferManager` を backend 内部に移す。公開 `WebGLRenderer` は廃止、`WebMeshRenderer` と
  `pictor_web_mesh` (ES module、配布済み) は GLES3 backend 上に載せ替えて JS 面の API を維持する。
  GLES3 backend は Emscripten 以外では ANGLE 経由の opt-in (`PICTOR_GLES3_ANGLE_DIR`) でデスクトップ
  検証できるようにする (必須ではない)。
- `pictor_webgl` ターゲットは廃止し、Emscripten では本体 `pictor` を `pictor_core` + backends で構成する
  (`portability/web.md` Stage 1 の `pictor_core` 切り出しを §6 P0 に含める)。
- **Ludellus-Native の wasm 化に必要な最低条件** (段階 5 の前提):
  1. `pictor` 本体が Emscripten でコンパイルできる (Vulkan 無条件 include の除去、`std::thread` の
     `SingleThreadDispatcher` 既定、同期 file I/O は MEMFS preload で通す)。
  2. WebGPU backend: canvas swapchain、buffer / texture / sampler、graphics pipeline (depth 付き)、
     compute pipeline (SDF アバター用)、bind group 4 space、instancing、timestamp は任意。
  3. GLES3 backend: 上記から compute / storage / indirect を除いた集合。降格宣言 (SDF → mesh) が
     Visus で解決されること。
  4. 非同期初期化の `begin_create / poll` と、`webglcontextlost` / `device lost` → `FrameResult` の配線。
  5. `c_api.h` v2 (§4.9) で canvas selector と backend 希望を渡せること。
  6. `pictor_compile_shaders` の出力 package が fetch で読めること (ハッシュ名・br/gz は Ludellus-Server 側)。

### 4.8 DX12 / Metal の既存実装の扱い (設計項目 8)

- §3.4 R3。**取り込まない**。確認した事実:
  - `git diff main feat/native-metal-dx12-recovery -- include/pictor/surface/dx12_context.h include/pictor/surface/metal_context.h` は空。
  - 2-dot `git diff --stat main feat/…` は 288 ファイル、+2,086 / −16,156 行。ブランチ側が持たないのは
    main の frame tap (`include/pictor/tap/*`)、XR、Visus v2 serializer、`context_init_result.h`、
    `frame_gate.h`、`vulkan_capability_check.h`、`vulkan_portability_probe.*`。
  - `git cherry main feat/…` の「main に無い」19 commit は 2026-09-16 の履歴再作成で patch-id が
    変わったもので、`surface/` の差分はブランチ側が古い (`vulkan_context.h` −90 行)。
- 種にするのは main の `Dx12Context` (`src/surface/dx12_context.cpp`、392 行) と `MetalContext`
  (`src/surface/metal_context.mm`、208 行)。swapchain / clear / 復旧状態機械を `src/rhi/d3d12/` と
  `src/rhi/metal/` の `ISwapchain` 実装へ移し、`NativeDrawCallback` / `NativeMeshView` は
  `ICommandEncoder` / `DrawPacket` に置き換えて削除する。`sharc_dx12_executor.cpp` は SHaRC モジュールの
  D3D12 variant として残し、RHI 化は SHaRC モジュール移行 (§6 P2b) で行う。
- ブランチは削除せず凍結 (`archive/` タグ)。これは履歴操作なので neco の指示を待つ (§8)。

### 4.9 公開 API と消費側の移行 (設計項目 9)

| 対象 | 今 | 後 | 互換 |
|---|---|---|---|
| `PictorRenderer::compile_render_graph(VkDevice, registries…)` | host が registries と device を渡す | `compile_render_graph()` (引数無し。Context と registries は Pictor 所有) | 旧 signature は interop 面に `[[deprecated]]` で 2 マイナー版 |
| `PictorRenderer::render_compiled(VkCommandBuffer, flight, image, PassRecordFn/recorder)` | host のコマンドバッファ記録中に呼ぶ | `render_frame(const Camera&)` が acquire → record → submit → present を Pictor 側で行う。host 記録が要る pass は `IRecordedPass` を登録 | 同上 |
| `VulkanContext` (host 所有) | 公開 | `include/pictor/rhi/vulkan/vulkan_context.h` (interop 面) に移動。`rhi::Context::native<VulkanContext>()` で取り出す | 2 マイナー版 |
| `ISurfaceProvider::get_required_instance_extensions` | 公開 virtual | 削除 | default 実装を残し呼ばれなくなる → 次の版で削除 |
| `c_api.h` v1 (`pictor_create_renderer_webgl(canvas)` / `_vulkan(window, instance)`) | 方式別 2 本 | v2: `pictor_create_renderer(const PictorSurfaceDesc*, PictorBackend preferred)` + `pictor_renderer_poll_init()` + `pictor_backend_report(…)`。`PICTOR_C_API_VERSION 2` | v1 の 2 本は v2 の wrapper として 1 版残す |
| `GpuTimerManager::initialize_vulkan(VulkanContext&)` | — | `initialize(rhi::IDevice&)` | interop 面 |
| GI / SHaRC / decal / UI / postprocess / XR / Rive の `initialize(VulkanContext&)` / `record(VkCommandBuffer)` | 公開 | モジュール単位で `initialize(rhi::IDevice&)` / `record(rhi::ICommandEncoder&)` へ (§6 P2b)。Rive と XR は manifest で backend 限定 | モジュールごとに 1 版 |
| Ergo `ergo_render` (`IRenderLayer::record(VkCommandBuffer, VkExtent2D)`、`FrameComposer::add_pass(VkRenderPass, …)`、`vk_fwd.h`) | Vk 公開 | Ergo 側で `IRenderLayer::record(pictor::rhi::ICommandEncoder&, Extent2D)` / `add_pass(pass_name)` に替え、`vk_fwd.h` を削除。`RenderBackendContract` は `pictor::rhi::BackendReport` の写しにする | Pictor から Ergo への依存は作らない。Pictor は拡張面を提供するだけ |
| Ergo `ergo_gpu_particle::IGpuBackend` | Ergo 独自の薄い interface | Ergo 側に `pictor::rhi::IDevice` 上の adapter を 1 つ置く (Vulkan / WebGPU の 2 adapter が 1 つになる) | — |
| Ludellus-Native `render/vk_resources`、`world_renderer` | Vk 直叩き | Ln 段階 2〜4 で `PictorRenderer` + Visus + `IRecordedPass` へ。`world/*` `app/*` の Vk include 0 件が Ln の受入 | — |

### 4.10 ビルド構成 (設計項目 10)

| option | 既定 | 条件 |
|---|---|---|
| `PICTOR_RHI_VULKAN` | ON (Vulkan SDK / NDK が見つかったとき) | 既存 `Vulkan_FOUND` |
| `PICTOR_RHI_D3D12` | ON on `WIN32` | Windows SDK 同梱。追加 SDK 不要 (既に `d3d12 dxgi` をリンク) |
| `PICTOR_RHI_METAL` | ON on `APPLE` (macOS / iOS) | `OBJCXX` |
| `PICTOR_RHI_WEBGPU` | ON on `EMSCRIPTEN`、それ以外 OFF | ネイティブは `PICTOR_WEBGPU_NATIVE_DIR` 指定時のみ |
| `PICTOR_RHI_GLES3` | ON on `EMSCRIPTEN`、それ以外 OFF | デスクトップは `PICTOR_GLES3_ANGLE_DIR` 指定時のみ |
| `PICTOR_RHI_DEFAULT` | 空 (= プラットフォーム既定順) | `PICTOR_BACKEND` 環境変数と同じ語彙 |

- **backend マクロは PRIVATE にする。** `PICTOR_HAS_VULKAN` 等が PUBLIC である限り公開ヘッダに
  `#ifdef` が残り、consumer のコンパイルが backend 構成で変わる。`PICTOR_RHI_*` は `target_compile_definitions(pictor PRIVATE …)`
  とし、consumer へ伝わるのは `PICTOR_HAS_RHI=1` (何か 1 つ以上ある) だけ。これが「消費側が方式を
  知らない」の build 側の担保。`PICTOR_HAS_VULKAN` は interop 面ヘッダの中でだけ使い、2 マイナー版で消す。
- backend が 0 個のビルドは headless (既存のテスト経路) として維持。
- SDK 方針: 取り込まない・暗黙 fetch しない (既存)。追加する外部物は (a) SPIRV-Cross (Vulkan SDK 同梱)、
  (b) DXC (Windows SDK / Vulkan SDK 同梱)、(c) Tint または Naga (手動配置、生成物コミットで CI 必須にしない)、
  (d) Dawn / wgpu-native prebuilt (opt-in)、(e) ANGLE prebuilt (opt-in)。`cmake/FindRive.cmake` と同じ
  「あれば使う、無ければその backend を OFF にして message」の形。
- CI:
  - Ubuntu GCC (既存): Vulkan + headless tests + **消費面検査** + **backend 0 個ビルド**。
  - Windows MSVC (追加): Vulkan + D3D12 + tests。`/utf-8` を test target ごとに付ける (既存メモ)。
  - Emscripten (追加): WebGPU + GLES3 のコンパイルのみ + `pictor_core` の headless tests を node で実行 (単体テストに限る)。
  - macOS (追加、要判断 §8): Metal コンパイル + MSL 再生成 diff。
  - GCC で落ちた前例への対策: `-Wall -Wextra -Wpedantic` を Linux ジョブで付ける、`<span>` / designated initializer の
    順序違い、`NOGDI` / `OPAQUE` マクロの Win 専用ガード、`std::aligned_alloc` の Android 代替、
    `__declspec` / `#pragma comment(lib)` の禁止を `coding-conventions` に追記する。

---

## 5. 「消費側が方式を知らない」の機械検査 (設計項目 11 の一部)

| 検査 | 方法 | 期待 |
|---|---|---|
| C1 公開ヘッダの方式固有識別子 | `tools/check_consumer_face.py`: `include/pictor/**` から `include/pictor/rhi/<backend>/**` を除いた全ヘッダを正規表現 `\b(Vk[A-Z]\w*|vk[A-Z]\w*|VK_[A-Z_]+|ID3D12\w*|IDXGI\w*|D3D12_\w*|MTL\w*|WGPU\w*|wgpu[A-Z]\w*|GL(uint|enum|sizei|int|float)|EMSCRIPTEN_\w*)\b` と `#include <(vulkan|d3d12|dxgi|Metal|webgpu|GLES3|emscripten)/` で走査 | **0 件**。P0 では現状 42 本を allowlist に入れ、段階ごとに減らし P4 で allowlist を空にする |
| C2 backend マクロの伝播 | `cmake --build` 後に `pictor` の `INTERFACE_COMPILE_DEFINITIONS` を `cmake -P` で読み、`PICTOR_RHI_*` / `PICTOR_HAS_(VULKAN|DX12|METAL|WEBGL)` が無い | 0 件 |
| C3 同一 consumer ソースの多構成ビルド | `samples/agnostic_host/` (1 ソース、`#ifdef` 禁止を C1 と同じ正規表現で検査) を backend 構成ごと (Vulkan のみ / D3D12 のみ / WebGPU のみ / GLES3 のみ / 0 個) にビルド・リンク | 全構成でビルド成功 |
| C4 interop 面の利用数 | C1 と同じ正規表現で `demo/` `tools/` `tests/` の include 数を数え、PR 本文に報告 | 減少のみ (増えたら理由を書く) |
| C5 降格の可視性 | headless テスト: Feature bit を欠いた fake `IDevice` で profile を compile し、`BackendReport::downgrades` に宣言どおりの項目が出る。宣言の無い欠落は error になる | 契約テスト |
| C6 hot path 不変条件 | `static_assert(sizeof(CompiledPass) <= 256)`、`DrawPacket` 64 B、`execute_compiled` 内の仮想呼び出し数を fake encoder で数える単体テスト (パスあたり ≤ 6) | 契約テスト |

---

## 6. 段階計画と受入条件 (設計項目 11)

各段階は単独 PR 群で main に入る。「既存 Vulkan 経路が退行しない」は、各段階で Ubuntu GCC の
headless tests が green、`fps_baseline_test` の基準値が維持され、Windows の demo (`demo/graphics`、
`demo/shadow_play`、Visus preview) をユーザが実機で起動確認することを指す。実機確認は本書の
著者は行えないため、各段階の受入欄に「実機: 未確認 (ユーザ確認待ち)」を既定で書く。

| 段階 | 内容 | 受入 (テスト) | 受入 (実機) |
|---|---|---|---|
| **P0 消費面の土台** | `tools/check_consumer_face.py` + allowlist、CI に C1/C2/C4 追加。`pictor.h` から Vulkan 系 include を外し `pictor_vulkan.h` へ。`pictor_core` (Vulkan 非依存部) を Emscripten でコンパイルできるようにする (`SingleThreadDispatcher`、`std::aligned_alloc` 代替)。backend マクロを PRIVATE 化する準備として `PICTOR_HAS_RHI` を追加 | C1 の allowlist 件数が確定。Emscripten ジョブで `pictor_core` がコンパイル。既存 tests green | 既存 demo 起動 (未確認) |
| **P1 rhi 抽象と Vulkan backend の抽出** | `include/pictor/rhi/*` を定義。`VulkanContext` を `src/rhi/vulkan/` の `IDevice` / `ISwapchain` / `ICommandEncoder` 実装にし、`rhi::Context` が所有。`FrameResult` 経路は共通部へ。外形の挙動不変 | `unit_surface_recovery_contract_test` / `unit_frame_contract_test` を `ISwapchain` 越しに通す。fake backend で `Context::create` の選択順序テスト | 既存 demo 起動、resize / 最小化の復旧 (未確認) |
| **P2 パイプライン系の RHI 化** | `CompiledPass` handle 化、registries の `initialize(IDevice&)`、`RenderTargetRegistry`、`Transition` side table、`PassRecordFn(ICommandEncoder&)`、`CompiledBatchRecorder` → `DrawPacket`、`GpuTimerManager::initialize(IDevice&)` | C6。`unit_compiled_graph_wiring_test` / `unit_pipeline_registries_test` / `unit_gpu_timer_test` を fake device で通す。`fps_baseline_test` 維持 | Visus preview / shadow_play が P1 と同じ絵 (未確認) |
| **P2b モジュール移行 (並行可)** | UI → postprocess → GI executor → decal → SHaRC → XR の順に `initialize(IDevice&)` / `record(ICommandEncoder&)`。Rive は Vulkan 限定 manifest | モジュールごとに C1 allowlist から外れる。manifest の backend 限定が error になる契約テスト | 該当 demo (未確認) |
| **P3 シェーダ変換基盤** | `pictor_shaderc` (glslang → SPIR-V → SPIRV-Cross / Tint or Naga、reflection)、`shaders/gen/`、可搬サブセット検査、space 規約への揃え直し、shader package v3 と `visus_migrate`、`pictor_compile_shaders` の export。Vulkan は引き続き SPIR-V を使うので挙動不変 | 全シェーダが全 backend へ変換できる (CI でツールがあるジョブは再生成 diff 空)。`unit_visus_shader_package_test` / `unit_shader_registry_test` を v3 で通す | 既存 demo (未確認) |
| **P4 選択・能力・降格・公開 API v2** | `BackendReport`、`DowngradeResolver`、profile / manifest / Visus の降格宣言、StatsOverlay・frame tap・ログの出力、`PictorRenderer` が Context 所有、`c_api` v2、backend マクロ PRIVATE 化完了、interop 面への旧 API 移動 | C1 allowlist が空、C2、C3 (Vulkan のみ / 0 個)、C5 | demo が `PICTOR_BACKEND=vulkan` と未指定で同じ絵 (未確認) |
| **P5 WebGPU backend** | `src/rhi/webgpu/`、非同期 `begin_create / poll`、canvas swapchain、ネイティブ opt-in、Emscripten で `PictorRenderer` + Visus の最小 demo (`demo/web_rhi/`) | C3 (WebGPU のみ)。ネイティブ Dawn があるとき headless の描画テスト (readback で 1 枚比較)。Emscripten ジョブでコンパイル | ブラウザ (Chrome / Edge 最新) で demo が描ける (未確認)。Dawn 手元検証 (未確認) |
| **P6 GLES3 backend と予備切替** | `src/rhi/gles3/`、`pictor_webgl` の内部移管、`WebMeshRenderer` 載せ替え、WebGPU 無し環境での自動降格と表示、Visus variant 降格 | C3 (GLES3 のみ)。C5 に WebGL2 の Feature 集合を追加。`unit_web_mesh_input_test` 維持 | WebGPU を無効にしたブラウザで予備が動き、画面隅に降格表示 (未確認) |
| **P7 D3D12 backend** | main の `Dx12Context` を種に `src/rhi/d3d12/`。root signature の space 対応、DXIL variant。`sharc_dx12_executor` を SHaRC モジュールの variant に統合 | C3 (D3D12 のみ)。Windows MSVC ジョブ。readback 1 枚比較 (Vulkan と同一シーン) | Windows で `PICTOR_BACKEND=d3d12` の demo (未確認) |
| **P8 Metal backend** | main の `MetalContext` を種に `src/rhi/metal/`。argument buffer、MSL variant、iOS / macOS provider | C3 (Metal のみ)。macOS ジョブ (要判断) | iPhone 15 以降 / Apple Silicon Mac (未確認。本リポには環境が無い) |
| **P9 消費側移行と整理** | Ergo `ergo_render` / `ergo_gpu_particle` adapter (Ergo 側 PR)、Ludellus-Native 段階 2〜4 との接続、interop 面の deprecated 削除、`feat/native-metal-dx12-recovery` の凍結、`dx12-backend-design.md` / `metal-backend-design.md` / `portability/*.md` / `platform-feature-matrix.md` の更新 | C4 の interop 利用数が 0。Ln の「`world/*` `app/*` に Vk include 0 件」 | Ludellus の `/mmo-wasm/` (未確認) |

依存: P0 → P1 → P2 → P3 → P4 → P5 → P6。P2b は P2 以降に並行。P7 は P3 以降に並行。P8 は P3 以降で
macOS 環境が得られたとき。P9 は P4 以降に順次。

---

## 7. リスク (推奨つき)

| # | リスク | 影響 | 推奨 |
|---|---|---|---|
| R1 | P1 / P2 の「挙動不変リファクタ」が最重量で、Vulkan 経路の退行を見逃す | 全 demo が壊れる | readback による 1 枚比較テスト (同一シーン・同一カメラ) を P1 で先に作る。`fps_baseline_test` を CI に入れる (今は headless 外) |
| R2 | SPIR-V → WGSL 変換の未対応命令 (subgroup / 64-bit / `gl_DrawID`) | WebGPU で特定シェーダが出ない | P3 の可搬サブセット検査で先に洗い出す。出ないシェーダはモジュール限定にして error |
| R3 | WebGPU の非同期初期化が既存の同期前提 (`initialize()` → bool) と噛み合わない | Web だけ起動列が違う = 消費側が方式を知る | `begin_create / poll` を全 backend 共通の列にする (§4.6)。`c_api` v2 に `poll_init` を入れる |
| R4 | `webgpu.h` の版差 (Dawn / wgpu-native / Emscripten) | ネイティブ検証と wasm で挙動差 | 版を 1 つに固定し `webgpu_compat.h` で吸収。CI は Emscripten の版を正にする |
| R5 | 生成シェーダのコミットでリポジトリが肥大 | clone 速度 | テキスト (WGSL / MSL / GLSL ES / HLSL) のみコミットし、DXIL / metallib / SPIR-V はビルド生成に限る |
| R6 | D3D12 の root signature と Vulkan の descriptor set の更新粒度差で、space 3 (DRAW) の更新が D3D12 で遅い | D3D12 だけ性能が出ない | space 3 は root CBV + instance SSBO の 1 本に固定し、descriptor table を触らない。実測は P7 受入 |
| R7 | WebGL2 で instance 頂点属性に落とすと、`RenderBatch` の instance 数上限と `DrawPacket` の意味が変わる | 予備だけ別経路 | `DrawPacket` の `first_instance` を backend 内で VBO offset に写す。消費側は不変 |
| R8 | macOS / iOS の環境が無く Metal は机上 | P8 が検証できない | P8 を独立段階にし、環境が得られるまで「未確認」のまま main に入れない (コンパイルも通せないため) |
| R9 | Ergo / Ludellus-Native の移行が Pictor の削除期限に追いつかない | interop 面が消せない | 削除は版数ではなく「C4 が 0 になったとき」に条件を変える |
| R10 | GCC で落ちる (MSVC は通る) | CI 赤 | Linux ジョブに `-Wall -Wextra -Wpedantic -Werror=return-type` を付け、P0 で現状の警告を潰す |
| R11 | `PICTOR_HAS_VULKAN` の PRIVATE 化で、同マクロを読む消費側 (Ergo `render_backend.h`) が黙って Vulkan 無しビルドへ落ちる | 実描画が消える | Ergo 側が `pictor::rhi::BackendReport` を読む形に変えるまで `PICTOR_HAS_RHI` を併用して PUBLIC に残す (P4 で切替) |

---

## 8. 未決事項 (推奨つき)

### 8.1 人間 (neco) にしか決められない点

| # | 事項 | 推奨 | 理由 |
|---|---|---|---|
| D1 | シェーダ正本を GLSL に据え置くか、Slang へ切り替えるか | **GLSL 据え置き (S1)**。Slang は P3 以降に「消費側カスタムシェーダの第 2 言語」として評価 | 既存 ≈ 100 本と CI 構成。Slang の WGSL 出力は実績が浅い |
| D2 | Windows の既定 backend (Vulkan 先か D3D12 先か) | **Vulkan 先** (現状維持)。D3D12 は P7 の実測後に見直す | 退行させない |
| D3 | デスクトップ OpenGL を作るか | **作らない** | §1 |
| D4 | macOS CI ランナー (有料) を増やすか | 増やさないなら P8 を「手元 Mac で検証できるまで保留」 | 本リポに macOS 環境が無い |
| D5 | `feat/native-metal-dx12-recovery` の凍結 (タグ付け・ブランチ削除) | 凍結タグを付け、削除は P9 まで待つ | 履歴操作 |
| D6 | 生成シェーダ (テキスト) のコミット可否 | **可** | §4.4、R5 |
| D7 | Dawn / wgpu-native / ANGLE の prebuilt を開発機に置く運用 | opt-in で置く (fetch しない) | §4.7 |
| D8 | Rive / XR を Vulkan 限定モジュールのままにするか | 当面 Vulkan 限定。manifest で明示 | 両方とも外部 SDK が Vulkan 前提 |
| D9 | WebGL2 で保証する機能の下限 (Ludellus が「予備」でどこまで遊べるか) | compute 系 (SDF アバター、GPU cull) 以外は同等。影は shadow map のまま、post-process は Bloom のみ | Ln 側の受入に影響 |
| D10 | wasm の `-pthread` (COOP/COEP) を Ludellus-Server で要求するか | 既定はシングルスレッド (`web.md` 決定 3) | 配信側の設定 |

### 8.2 設計で決めたが実装時に見直しうる点 (前提未確定)

- `DrawPacket` 64 B の中身 (bind group 2 本で足りるか。skinning の bone buffer を space 3 に入れる前提)。
- space 1 (PASS) の bind group を compile 時に固定できるか (post-process chain の中間ターゲットは
  pass ごとに変わるため、side table の slot 配列にする可能性)。
- WebGPU の `timestamp-query` は Chrome で量子化 (100 µs) されるため、profiler の表示を backend 別に
  「精度」付きにする。
- Metal の D24S8 不在により既定 preset の depth 形式を D32S8 に寄せるか (Vulkan でも可)。
- 非同期初期化の `poll()` を `PictorRenderer::initialize()` の戻り値にどう折り込むか (`InitState` 列挙を返す案)。

---

## 9. 関連

- `spec/feature/dx12-backend-design.md` / `metal-backend-design.md` (本書で置き換える段階計画)
- `spec/feature/portability.md` / `portability/web.md` / `portability/mobile.md` / `portability/mobile-surface-recovery.md`
- `spec/feature/platform-feature-matrix.md` (P9 で更新)
- `spec/architecture/framework-boundaries.md` (PC-RULE-001 〜 003)
- `spec/feature/visus-v2-design.md` §2.5 (shader package)
- `spec/feature/frame-tap.md` (header への backend / downgrades 追加)
- Ludellus-Native `spec/feature/native-first-web.md` / `spec/tasks/2026-10-14-native-first-web.md`
- Ergo `include/ergo/render/*` / `include/ergo/gpu_particle/gpu_backend.h`
