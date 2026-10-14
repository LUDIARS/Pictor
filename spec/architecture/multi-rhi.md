# 描画方式の切り替え口 (RHI) — 統合設計

- 日付: 2026-10-14
- 対象: Pictor (Pc)
- Actio: `a38aab5b-3141-4093-a108-a71722bd25b0` (Ludellus-Native の Native 一本化 段階 1)
- 依頼: `spec/tasks/2026-10-14-multi-rhi-design-request.md`
- 素案: `spec/plan/2026-10-14-multi-rhi-fable.md` (Fable)、`spec/plan/2026-10-14-multi-rhi-opus.md` (Opus。依頼文の「Astra」側。
  Astra は Codex sandbox の初期化失敗で起動できず、Opus に差し替えた)
- 種別: 設計 (正本)。実装・ビルド・実機確認は含まない。既存 `spec/feature/dx12-backend-design.md` §3/§6 と
  `spec/feature/metal-backend-design.md` の RHI / 段階計画、`spec/feature/portability/web.md` 決定 2 は本書で置き換える。

人間の指示 (neco 2026-10-14): 「PictorのレンダリングAPIにDirectX、Metal、WebGPU/GLの対応入れる」「描画は抽象化してそれぞれの
プラットフォームでなんも考えなくていいように」。Web は WebGPU 主・WebGL2 予備 (Ludellus-Native の決定)。

---

## 0. 比較と採否

| 論点 | Fable 案 | Opus 案 | 採用 | 理由 |
|---|---|---|---|---|
| 層構成 | 消費面 / 拡張面 (`ICommandEncoder` を公開) / interop 面の 3 段 | 公開面 `pictor/gfx/` + 内部 RHI (`src/rhi/`、非公開) + interop | **Opus** | 「なんも考えなくていい」に対し、仮想 encoder を消費側へ出すと WebGL2 での compute 記録など能力差が消費側で起きる。RHI は内部に閉じる |
| 消費側の独自描画 | `IRecordedPass` + 仮想 `ICommandEncoder` | `PassRecorder` (POD コマンド列を追記、方式が再生) | **Opus** | 仮想呼び出しゼロ、headless で記録内容を検査でき、5 方式で同じ |
| 内部 RHI の仮想粒度 | パス単位 (begin / bind / packets / end) | フレーム 1 回の `execute` + 方式内の具象 recorder | **両方の要素**: 内部 RHI は責務別インターフェース (Fable/Opus 共通)、hot path は方式内具象ループ。仮想は「パス単位まで」を上限とし draw / bind 単位はゼロ | どちらも per-draw 仮想ゼロで一致。モジュール移行のしやすさから「パス単位まで可」とする |
| 方式の選択 | ビルドで同梱、実行時に選択 | 同左 | 一致 | — |
| 明示指定した方式が使えないとき | 次の候補へ | 明示エラー (`allow_fallback` で許可) | **Opus** | 上書きは開発・検証の意図。黙って別方式で動かない |
| 能力の型 | Feature bits + Limits | Tier (A/B/C) + Feature bits + Limits | **Opus** | Tier は降格表と preset の検査を短く書ける |
| 降格 | profile / manifest / Visus に `requires` + `on_missing` | `requires` + `fallback` (ID or `omit`)、Emulated も記録 | **Opus を基に Fable の出力 4 箇所** | 宣言の無い欠落はエラー (両案一致)。記録はログ・StatsOverlay・frame tap・`BackendReport` |
| バインディング | space 4 つ固定 (FRAME/PASS/MATERIAL/DRAW)、push は backend 内で UBO ring に落とす | group 4 つ固定、**push constant を正本で使わない** (group 3 動的 UBO) | **Opus** | Vulkan でも同じ形にして方式差を作らない |
| シェーダ正本 | GLSL (Vulkan 方言) + SPIR-V 経由変換 | 同左 (Slang / HLSL は後で入口を足せる) | 一致 | 既存 ≈ 100 本を書き直さない |
| WGSL 変換 | Tint または Naga | Tint | **Tint** | Dawn / emdawnwebgpu と同系統 |
| 生成物のコミット | テキスト生成物 (WGSL/MSL/GLSL ES/HLSL) をコミット、CI で再生成 diff | コミットしない、`PICTOR_SHADER_BUNDLE_DIR` で生成済み束を渡す | **Fable (テキストのみコミット)** — 未決 U-6 | 消費側と GCC CI に Tint / DXC / Metal toolchain を要求しない。SPIR-V / DXIL / metallib は生成のみ |
| render pass | begin で attachment 列 (dynamic rendering 型) | `RenderTargetSet` + 並列表 | 一致 (表現は Opus の `RenderTargetSet`) | — |
| 描画データ | `DrawPacket` 64 B の AoS | `DrawStream` SoA | **Opus (SoA)** | Pictor の DoD 規約 (SoA 基本) |
| CompiledPass | handle 化、≤ 256 B 維持 | 方式中立 POD、≤ 128 B 目標、Vk 実体は方式の並列配列 | **Opus** | 実体を方式側の並列配列に置く方が CompiledPass から方式が完全に消える |
| post-process | host_recorded のまま、モジュールを RHI 化 | preset 展開時に N 個の通常パスへ開く | **Opus** | host 記録の特例を消す。移行中は Vulkan 限定 manifest で明示 (Fable の手当て) |
| DeviceLost | 既存 `FrameResult` 契約 | 同左 + **ハンドルを保ったまま実体を作り直す** (出所記録 / `ResourceLost` 通知) | **Opus** | 消費側がハンドルを作り直さない |
| 非同期初期化 (WebGPU) | `begin_create / poll` を全方式共通の列に | `initialize()` が Web で `Pending`、`poll_initialize()` | **Fable** | 全方式で同じ呼び出し列 = 消費側が方式を知らない |
| `feat/native-metal-dx12-recovery` | 取り込まない (内容は main と同一、残差は main より古い) | 同左 | 一致 | 親の前提 (main 未反映) は誤り。両案が独立に確認 |
| デスクトップ OpenGL | 作らない | 作らない | 一致 | — |
| WebGL2 | RHI の対象にする (`web.md` 決定 2 を改める) | 同左、Tier A | 一致 | — |
| 公開マクロ | `PICTOR_HAS_*` を PRIVATE 化、consumer には `PICTOR_HAS_RHI` のみ | 方式マクロは PRIVATE、公開ヘッダは方式マクロ非依存 | 一致 | Ergo の `render_backend.h` が読んでいるので切替は S4 (Fable R11) |
| 検査 | C1〜C6 (公開ヘッダ識別子・マクロ伝播・同一ソース多構成ビルド・interop 利用数・降格・hot path) | 公開ヘッダ検査・マクロ非依存単独コンパイル・同一サンプル host 行列 | **Fable の C1〜C6** (Opus の単独コンパイル検査を C2 に足す) | 機械検査が最も具体的 |
| 段階 | P0〜P9 (10 本) | S0〜S8 (9 本) | §6 に統合 | どちらも「公開面と検査 → Vulkan 移設 → シェーダ → Web → DX12/Metal → 撤去」 |

---

## 1. 境界

```
消費側 (Ergo / Ludellus-Native / 非ゲーム host)
   │  include/pictor/gfx/*.h   方式の型・名前・マクロを含まない公開面
   ▼
RenderSystem (公開)            方式の選択、BackendReport、資源ハンドル、フレームの開始/終了、PassRecorder の受付
   │
FrameGraph 実行 (内部)         CompiledGraph (方式中立 POD) + DrawStream (SoA) + PassRecorder 列
   │
src/rhi/common/                選択・能力表・降格解決・ハンドル表・frame gate・復旧の共通部
src/rhi/{vulkan,d3d12,metal,webgpu,webgl2,null}/   方式の型はここだけ (PRIVATE)
include/pictor/backend/<方式>_interop.h            移行期間の相互運用 (opt-in、検査の許可リストで別扱い)
```

### 公開面 (`include/pictor/gfx/`)

`RenderSystem` / `RenderSystemConfig` (方式の希望順・`allow_fallback`)、`BackendKind` (読むだけ)、`DeviceCaps`
(Tier + Feature bits + Limits、読むだけ)、`BackendReport` (選ばれた方式・試行・降格)、世代付き 32bit ハンドル
(`BufferId` / `TextureId` / `SamplerId` / `PipelineId` / `BindGroupId` / `RenderTargetSetId`)、記述子
(`BufferDesc` / `TextureDesc` / `SamplerDesc` / `PipelineDesc`)、`GfxFormat` / `LoadOp` / `StoreOp`、
`ShaderBundleRef`、`PassRecorder` / `PassContext`、既存の方式中立型 (`FrameResult` / `FrameStatus` /
`ContextInitResult` / `ISurfaceProvider` / `NativeWindowHandle` (+ `WebCanvas`、`MetalLayer`))。

閉じるもの: RHI 一式、方式の device / queue / swapchain / heap、形式変換、シェーダバイナリの選択、native encoder。

### 内部 RHI (`src/rhi/`、責務別。太い `IRenderer` は作らない)

| インターフェース | 責務 |
|---|---|
| `IRhiDevice` | 生成・破棄、能力照会、資源 / パイプライン生成、`compile_graph`、パス列の実行 |
| `IRhiSwapchain` | acquire / present / resize → `FrameResult` |
| `IRhiResourceTable` | ハンドル → 方式実体 (init / 生成時のみ伸びる。hot path は添字直引き) |
| `IRhiUploader` | staging / UMA 直書き / `writeBuffer` / `bufferSubData` の差 |
| `IRhiQueryPool` | timestamp (`GpuTimerManager` の下回り) |
| 方式内具象 recorder (`final`、非仮想) | DrawStream と PassRecorder 列の再生 |

hot path: CompiledGraph / DrawStream / PassRecorder 列は string / map / 方式ハンドルを含まない flat 配列。
仮想呼び出しはパス単位まで。draw / bind 単位はゼロ (C6 で数える)。

---

## 2. 能力差と降格

- `DeviceCaps { CapabilityTier tier (A_Raster=WebGL2 / B_Compute=WebGPU / C_Full=Vulkan,D3D12,Metal); features; limits; clip_space; depth_formats }`。
  方式ごとの既定表は Fable §4.2 / Opus §3.2 (一致)。実値は各方式で照会する。
- **降格は宣言で持つ**: render pass (profile)・機能モジュール manifest・Visus variant が
  `requires: [...]` と `fallback: <ID> | "omit"` を宣言する。
- 解決はグラフのコンパイル時 (初期化・profile 切替・方式切替) に 1 回。fallback を再帰で辿り、満たせなければ
  **コンパイル失敗** (`MissingCapability`)。`omit` は宣言した場合だけ。Pictor が別の描画を発明しない。
- Emulated (見た目同じで実装が変わる: push → UBO、indirect count → CPU 確定、first instance 補正) も記録する。
- 記録先 (4 箇所): 起動ログ 1 回 / StatsOverlay の 1 行 / frame tap の header (`frame-tap.md` で版を上げる) /
  `RenderSystem::report()`。
- 消費側は降格を書かない。書くのは素材 (Visus variant) と省略可否の宣言だけ。方式名での分岐 API は提供しない。

## 3. バインディング

- group 4 つ固定 (WebGPU の `maxBindGroups = 4`): 0 FRAME / 1 PASS / 2 MATERIAL / 3 DRAW (動的オフセット UBO 1 本)。
- push constant は正本で使わない (既存シェーダは S3 で group 3 へ移す)。テクスチャとサンプラは分けて宣言。
- 宣言はシェーダ反射 (SPIR-V) から生成し、シェーダ束に同梱する。手書きの layout は持たない。
- 方式対応表は Opus §3.3 (D3D12 = descriptor table + group 3 は root CBV、Metal = 当初は直接添字、
  WebGPU = bind group、WebGL2 = 束生成時に UBO binding point / texture unit へ平坦化)。bindless は Tier C 任意機能。

## 4. シェーダ

- 正本: GLSL 450 (Vulkan 方言)。経路: glslang → SPIR-V → SPIRV-Cross (GLSL ES 3.00 / MSL / HLSL→DXC→DXIL)、
  SPIR-V → Tint (WGSL)。反射は SPIR-V から。
- ビルド時ツール `pictor_shaderc` が 1 ソースからそのビルドで ON の方式分を作り、**シェーダ束** (方式別 blob + 反射 JSON、版番号付き) にまとめる。
- テキスト生成物 (WGSL / MSL / GLSL ES / HLSL) はコミットし、ツールがある CI ジョブで再生成 diff を検査する (U-6)。
  SPIR-V / DXIL / metallib はビルド生成のみ。
- Web は実行時コンパイルしない。実行時コンパイルはデスクトップ開発用の任意モジュール (`PICTOR_SHADER_HOT_COMPILE`)。
- 可搬サブセットを lint で強制 (tessellation / geometry / subpass input / 64bit atomic / `gl_DrawID` / 大きな push / bindless 配列 /
  storage の `vec3`)。満たさないシェーダはモジュール manifest で方式限定 (例: ocean の tessellation)。
- Visus: `VisusShaderRef` の解決先をシェーダ束にし、variant に `requires` を足す。`shaderpkg` は版を上げ `visus_migrate` で変換。
- 消費側: `pictor_add_shader_bundle(<target> SOURCES ...)` (CMake 関数を export)、実行時は `ShaderBundleRef{"name"}`。
- `shaders/webgl/*` と `shaders/sharc/hlsl/*` は生成物が安定したら削除 (それまで方式別上書きとして許可し、使ったことを束のメタデータに残す)。

## 5. パイプライン系・表示面・Web・既存実装・消費側

- **CompiledPass**: 方式中立 POD (Opus §3.5 の形、≤ 128 B 目標、`static_assert`)。Vk の実体は Vulkan 方式の並列表。
  レイアウト遷移はグラフが計算する `TransitionOp` 範囲 (side table)。input attachment / subpass は使わない。
- **post-process**: preset 展開時に通常パスへ開く。移行完了までは Vulkan 限定 manifest で、他方式では明示エラー。
- **独自パス**: `void(PassRecorder&, const PassContext&)`。Tier A で `Dispatch` を記録したらコンパイル時 (宣言漏れなら記録時) の明示エラー。
  旧 `PassRecordFn(VkCommandBuffer, ...)` は `vulkan_interop.h` に移して互換期間を置く。
- **表示面**: `ISurfaceProvider` 維持。`get_required_instance_extensions()` は Vulkan 方式が handle から導き、interface から外す。
  方式別 swapchain と `FrameStatus` の対応は Opus §3.6。sticky loss / suspended は `src/rhi/common/` へ。
  DeviceLost は RenderSystem 内で復旧し、ハンドルは保つ (Pictor ローダ由来は出所から再読込、消費側が渡した資源は `ResourceLost` 通知。U-4)。
- **初期化**: 全方式で `begin_initialize()` → `poll_initialize() → Pending | Ready | Failed` の同じ列。ネイティブは begin 内で同期完了。
- **自動選択**: 既定順 Windows = Vulkan → D3D12、Linux = Vulkan、macOS = Vulkan(MoltenVK) → Metal、iOS = Metal、Android = Vulkan、
  Web = WebGPU → WebGL2 (U-1)。上書きは `RenderSystemConfig::backend_preference` / 環境変数 `PICTOR_BACKEND`。
  全試行を `BackendReport::attempts` に残す。
- **WebGPU**: 標準 `webgpu.h` に書く。Web は `--use-port=emdawnwebgpu`、ネイティブは利用者が用意した Dawn prebuilt (`PICTOR_DAWN_DIR`) のときだけ。fetch しない。
- **WebGL2**: `pictor_webgl` の context / shader / buffer を `src/rhi/webgl2/` の下回りへ移す。`WebMeshRenderer` と配布済み
  `pictor_web_mesh` の JS 契約は保ったまま `RenderSystem` 経由へ。`WebGLRenderer` (icosphere 参考実装) は廃止。
- **Ludellus-Native wasm の最低条件**: Fable §4.7 と Opus §3.7 の和 (pictor_core の Emscripten 構成、SingleThreadDispatcher、
  WebGPU / WebGL2 の両方式、束に両方式分、非同期初期化、MEMFS 資産、`BackendReport` を画面隅へ出せる、c_api v2)。
- **`feat/native-metal-dx12-recovery`**: 取り込まない。main の `Dx12Context` / `MetalContext` を `src/rhi/d3d12/` / `src/rhi/metal/`
  の素体にする。`NativeDrawCallback` / `NativeMeshView` は撤去 (互換期間あり)。ブランチの凍結・削除は U-5。
- **公開 API 移行**: `compile_render_graph(VkDevice, ...)` / `render_compiled(VkCommandBuffer, ...)` → `RenderSystem` の
  begin_frame / render / end_frame。`VulkanContext` は interop へ。`c_api.h` v2 (`pictor_create_renderer(surface, preference)` +
  `poll_init` + `backend_report`、v1 は転送で 1 版残す)。Ergo `ergo_render` の移行表は Opus §3.9 (Ergo 側の作業。Pictor → Ergo 依存は作らない)。
- **ビルド**: `PICTOR_RHI_{VULKAN,D3D12,METAL,WEBGPU,WEBGL2,NULL}`。既定は Fable §4.10 / Opus §3.10 (一致)。方式 0 個は headless (null)。
  方式マクロは PRIVATE、consumer に出るのは `PICTOR_HAS_RHI` のみ (S4 で切替、それまで `PICTOR_HAS_VULKAN` を併存)。
  依存 SDK は取り込まない・暗黙 fetch しない。DXC が無ければ D3D12 を OFF にする案内付きで configure 時に明示。
- **CI**: Ubuntu GCC (既存) に C1/C2/C4 と null 方式ビルドを追加、`-Wall -Wextra -Wpedantic`。Windows MSVC (Vulkan + D3D12)、
  Emscripten (WebGPU + WebGL2 のコンパイル + pictor_core の node 単体テスト)、macOS は U-3。

---

## 6. 段階計画と受入条件

各段階は単独で main に入り、既存 Vulkan 経路を退行させない (Ubuntu GCC の headless tests green、`fps_baseline_test` 維持、
Windows demo の実機確認は人間。実機欄は既定で「未確認」)。

| 段階 | 内容 | 受入 (テスト) |
|---|---|---|
| **S0 公開面・検査・null 方式** | `include/pictor/gfx/` の型、null 方式 (記録のみ)、`PassRecorder` (POD 列)、C1 (公開ヘッダ識別子、現状 42 本を allowlist)、C2 (マクロ伝播 + 方式マクロ未定義での単独コンパイル)、C4、`pictor.h` から Vulkan include を外し `pictor_vulkan.h`、`pictor_core` を Emscripten でコンパイル (SingleThreadDispatcher、`aligned_alloc` 代替)、`PICTOR_HAS_RHI` 追加 | C1 allowlist 件数の確定、C2、PassRecorder の記録内容テスト、Emscripten コンパイル、既存 tests green |
| **S1 Vulkan を口の内側へ** | `src/rhi/vulkan/` に VulkanContext・GpuTimer・scheduler 記録を移す。`rhi::Context` 所有。`FrameResult` 経路を共通部へ。外形の挙動不変。readback 1 枚比較テストを先に作る | surface recovery / frame contract テストを `IRhiSwapchain` 越しに通す、選択順序テスト (fake)、readback 比較 |
| **S2 パイプライン系** | CompiledPass 方式中立化、`RenderTargetSet`、`TransitionOp`、DrawStream、registries の `initialize(IRhiDevice&)`、GpuTimer の RHI 化 | C6 (`sizeof`、パスあたり仮想回数)、compiled graph / registries / gpu timer テストを fake device で、`fps_baseline_test` |
| **S2b モジュール移行 (並行)** | UI → post-process (通常パスへ展開) → GI → decal → SHaRC → XR。Rive / XR は Vulkan 限定 manifest | モジュールごとに C1 allowlist から外れる、manifest の方式限定がエラーになる契約テスト |
| **S3 シェーダ束** | `pictor_shaderc`、lint、push → group 3、束形式と反射、Visus 解決先と `visus_migrate`、`pictor_add_shader_bundle`、テキスト生成物のコミットと再生成 diff | 全シェーダが lint 通過、SPIR-V 反射が S2 と一致、束読込テスト、Visus と反射の不一致がエラー |
| **S4 選択・能力・降格・公開 API v2** | `BackendReport`、`DowngradeResolver`、宣言 (`requires` / `fallback` / `omit`)、4 箇所への出力、`RenderSystem` が Context 所有、c_api v2、方式マクロ PRIVATE 化、旧 API を interop へ | C1 allowlist 空、C2、C3 (Vulkan のみ / null)、C5 (能力表を差し替えた降格解決) |
| **S5 WebGPU** | `src/rhi/webgpu/`、非同期初期化、canvas swapchain、Dawn opt-in、`demo/web_rhi/` 最小 host (ゲーム非依存) | C3 (WebGPU のみ)、Emscripten コンパイル、Dawn がある環境で readback 比較 |
| **S6 WebGL2** | `src/rhi/webgl2/`、`pictor_webgl` 下回りの移管、`WebMeshRenderer` 載せ替え、WebGPU 不在時の自動降格と表示 | C3 (WebGL2 のみ)、C5 に Tier A、web-mesh の既存テスト維持 |
| **S7 D3D12** | `Dx12Context` を素体に `src/rhi/d3d12/`、root signature、DXIL 束、SHaRC の束化 | C3 (D3D12 のみ)、Windows MSVC、Vulkan と同一シーンの readback 比較 |
| **S8 Metal** | `MetalContext` を素体に `src/rhi/metal/`、MSL 束、macOS / iOS provider | C3 (Metal のみ)、macOS ジョブ (U-3)。環境が無ければ「未確認」のまま main に入れない |
| **S9 消費側移行と撤去** | Ergo `ergo_render` / `ergo_gpu_particle` (Ergo 側 PR)、Ludellus-Native 段階 2〜4 と接続、interop の旧経路・`NativeDrawCallback`・`get_required_instance_extensions`・旧 c_api を削除、既存 spec (dx12 / metal / portability / platform matrix) を更新 | C4 の interop 利用数 0、Ln の「`world/*` `app/*` に方式 include 0 件」 |

実装記録: S0 は [`spec/tasks/2026-10-14-multi-rhi-s0-public-face.md`](../tasks/2026-10-14-multi-rhi-s0-public-face.md)
(Actio `9998f9a7`)。C1 の allowlist は実測 41 本 (コメント除去・トークン単位)、検査は `tools/consumer_face/`。

依存: S0 → S1 → S2 → S3 → S4 → S5 → S6。S2b は S2 以降に並行。S7 は S3 以降に並行。S8 は S3 以降で環境が得られたとき。S9 は S4 以降に順次。
S5 と S6 は入替可 (降格規則は先に入れる方で実装)。

### 機械検査 (「消費側が方式を知らない」)

| 検査 | 内容 |
|---|---|
| C1 | `include/pictor/**` (interop を除く) に方式固有識別子・include が 0 件 (トークン検査、コメント除去) |
| C2 | `pictor` の `INTERFACE_COMPILE_DEFINITIONS` に方式マクロが無い + 公開ヘッダを方式マクロ未定義で単独コンパイル |
| C3 | ゲーム非依存のサンプル host 1 ソースを方式構成ごと (Vulkan / D3D12 / WebGPU / WebGL2 / null) にビルド。ソースに方式分岐 0 件 |
| C4 | `demo/` `tools/` `tests/` の interop include 数を PR に報告 (減少のみ) |
| C5 | 能力を欠いた fake device で profile をコンパイルし、宣言どおりの降格 / 宣言の無い欠落はエラー |
| C6 | `sizeof(CompiledPass)`、実行時のパスあたり仮想呼び出し回数 (fake で数える) |

---

## 7. リスク

両案のリスク表 (Fable §7 R1〜R11、Opus §5 R-1〜R-10) を引き継ぐ。最重要は次の 4 つ。

- S1 / S2 の挙動不変リファクタで Vulkan 経路の退行を見逃す → readback 1 枚比較と frame tap の描画リスト突き合わせを S1 で先に作る。
- `PICTOR_HAS_VULKAN` の PRIVATE 化で Ergo `render_backend.h` が黙って Vulkan 無しに落ちる → S4 まで併存し、Ergo 側の移行 PR を S4 と同時に出す。
- SPIR-V → WGSL / DXIL の未対応命令 → S3 の lint で先に洗い出し、方式限定 manifest で明示エラー。方式別上書きソースは記録付きで許可。
- macOS / iOS の環境が無く Metal が机上 → S8 を独立段階にし、検証できるまで main に入れない。

## 8. 未決事項 (推奨つき)

人間 (neco) にしか決められないもの。いずれも S0〜S2 の着手は妨げない (推奨で進め、該当段階の前に確定する)。

| ID | 論点 | 推奨 | 決める時期 |
|---|---|---|---|
| U-1 | Windows / macOS の既定方式 | Windows は Vulkan 先、macOS は MoltenVK 先。D3D12 / Metal が全 preset 対応後に見直す | S7 / S8 の受入時 |
| U-2 | シェーダ正本を将来 Slang へ移すか | GLSL 据え置き。方式別上書きが常態化したら再判断 | S3 後 |
| U-3 | macOS / Windows の CI ランナー費用 | S7 / S8 開始時にコンパイルのみで導入 | S7 前 |
| U-4 | DeviceLost 時に CPU 側の写しを Pictor が持つか | 既定は持たない (ローダ由来は出所から再読込、消費側資源は `ResourceLost` 通知)、`retain_cpu_copy` で選べる | S4 前 |
| U-5 | `feat/native-metal-dx12-recovery` の凍結・削除 | 凍結タグを付け、削除は S9 まで待つ (履歴操作) | 任意 |
| U-6 | テキスト生成シェーダのコミット | コミットする (消費側・GCC CI にツールを要求しない) | S3 前 |
| U-7 | WebGL2 予備で保証する下限 | compute 系 (SDF アバター、GPU cull) 以外は同等。影は shadow map、post は Bloom のみ | S6 前 (Ludellus-Native の受入に影響) |
| U-8 | 互換期間の終わり | Ergo と Ludellus-Native が新公開面へ移った時点 (C4 = 0)。日付では決めない | S9 |
