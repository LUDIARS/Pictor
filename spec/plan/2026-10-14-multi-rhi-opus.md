# 設計案: 描画方式の切り替え口 (RHI) — Vulkan / DirectX 12 / Metal / WebGPU / WebGL2

- 日付: 2026-10-14
- 対象: Pictor (Pc)
- 依頼文: `spec/tasks/2026-10-14-multi-rhi-design-request.md` (commit 7dcffea) の「Astra」側の独立案
- Actio: `a38aab5b-3141-4093-a108-a71722bd25b0` (Ludellus-Native の Native 一本化 段階 1)
- 状態: **設計のみ。実装・ビルド・テスト・実機確認はしていない。**
- 確認基準: この作業場所 (HEAD `70733c5`、origin/main に含まれる) とローカル `main`、ブランチ `feat/native-metal-dx12-recovery` (head `847b56c`)

---

## 0. 結論

1. **公開面と RHI を分ける 2 層にする。** 消費側が触るのは方式を含まない公開面 (`pictor/gfx/`) だけで、
   RHI (`IRhiDevice` ほか) は Pictor の内側 (`src/rhi/`、 公開ヘッダに出さない) に置く。消費側が
   RHI を直接叩く設計にはしない。RHI を公開すると「方式を知らない」は守れても、能力差の判断が消費側へ漏れるため。
2. **方式はビルドで同梱を決め、実行時に同梱分から選ぶ。** 仮想関数は「フレームに 1 回 / パスに 1 回」の
   粗い粒度だけに置き、draw 単位の記録は各方式の中の直値ループで行う。CompiledGraph は方式中立の
   添字配列にし、Vk ハンドルは Vulkan 方式が持つ並列配列へ移す。
3. **能力は 3 段 (Tier) + 個別フラグで照会し、降格はデータで宣言する。** パス・モジュール・Visus が
   `requires` と `fallback` を宣言し、グラフのコンパイル時に Pictor が解決する。結果は
   `BackendReport` (選ばれた方式・降格一覧) として記録・FrameTap・画面隅に出す。代替の宣言が無ければ明示エラー。
4. **シェーダは SPIR-V を中間に固定する。** 今の GLSL 資産 (約 84 本) はそのまま正本として使い、
   ビルド時に SPIR-V → WGSL (Tint) / GLSL ES 3.00 / MSL (SPIRV-Cross) / DXIL (SPIRV-Cross の HLSL → DXC) を生成する。
   Web では実行時コンパイルをしない。Slang / HLSL は SPIR-V を出せる「追加の入口」として後から足せる。
5. **`feat/native-metal-dx12-recovery` は取り込まない。** 親の前提と違い、DX12 / Metal の描画面と復旧経路
   (`dx12_context` / `metal_context` / `native_render_config`、SHaRC DX12、const ラムダ修正) は
   main に同じ内容で入っている (下記 §1.3)。残りの差は古い先祖側の内容で、取り込むと退行する。
6. **段階は「消費側から見える公開面を先に固める」順にする。** S0 公開面と検査 → S1 Vulkan を口の内側へ (挙動不変)
   → S2 シェーダ束 → S3 WebGPU → S4 WebGL2 → S5 Pictor 本体の wasm 構成 → S6 DX12 → S7 Metal → S8 旧 API の撤去。
   親の順序 (Vulkan → WebGPU → WebGL2 → DX12/Metal) に同意し、その前に S0 と S2 を独立段階として足す。

---

## 1. 解釈 (親の解釈への同意・異論)

### 1.1 同意する点

- 「GL」は Web の予備としての WebGL2 (OpenGL ES 3.0 相当) と読む。
- 最上位の受入条件は「消費側 (ゲーム・Ergo・非ゲーム host) のコードが描画方式を一切知らない」。
  方式固有の型・分岐・シェーダの書き分けを消費側に要求せず、降格の判断も Pictor が行う。消費側は結果を読むだけ。
- Ludellus-Native の wasm 化 (WebGPU 主 / WebGL2 予備) が当面の主目的なので、Web 2 方式を DX12 / Metal より先にする。
- 既存 `dx12-backend-design.md` の `IRhi*` 案 (責務別の薄いインターフェース) は出発点として使える。
  ただし同書 §6 の「Phase 0 で RHI 抽出」は公開面を決めずに内側だけ切る順序で、消費側の移行先が定まらない。
  本案は公開面 (§3.9) を先に固める点で順序を変える。

### 1.2 補足・解釈を広げる点

- **「消費側が方式を知らない」は C++ 型だけでなくデータにも及ぶ。** Visus / preset / パス宣言に
  `"backend": "vulkan"` のような分岐を書かせない。方式差はパス・モジュールの能力要求 (`requires`) で表し、
  方式名ではなく能力で降格する。方式名で分岐してよいのは Pictor 内部と開発用の上書き設定だけ。
- **ただし消費側が「代替の素材」を用意することは許す。** たとえば Ludellus の SDF アバター (compute) が
  WebGL2 で三角形アバターに落ちる件 (`Ludellus-Native/spec/feature/native-first-web.md`) では、
  三角形アバターの素材は消費側にしか作れない。これは Visus の variant として**データで**宣言し、
  どちらを使うかは Pictor が能力から決める。消費側に `if (webgl)` を書かせないことが要点で、素材を 1 種に限ることではない。
- **既存 `portability/web.md` 決定 2 の「WebGL2 は RHI の対象にしない」は改める。** あれは WebGPU だけを
  本命とする前提だった。neco の指示は「WebGL2 も同じ消費側コードで動く予備」なので、WebGL2 も同じ口に並べ、
  能力の最下段 (Tier A) として扱う。WebGL2 が対応しない機能は降格規則で処理する。
- **デスクトップ OpenGL は作らない (推奨)。** 理由は §9.3。ただし WebGL2 方式のコードは GLES 3.0 と大部分が
  共通なので、将来 Android の GLES3 予備が必要になったときの足場としては残る。

### 1.3 異論: DX12 / Metal 実装は main に入っている

依頼文 §2 の「DX12 / Metal の描画面 (surface/present) と復旧経路は `feat/native-metal-dx12-recovery`
ブランチに実装があるが main 未反映」は、この作業場所で確かめた事実と合わない。

| 確認 | 結果 |
|---|---|
| `git diff main feat/native-metal-dx12-recovery -- src/surface/dx12_context.cpp src/surface/metal_context.mm src/gi/sharc_dx12_executor.cpp include/pictor/surface/{dx12_context,metal_context,native_render_config}.h` | 差分 0 |
| main の `dx12_context.cpp` の履歴 | `a6f42b9 native Metal と DX12 の描画面および復旧経路を実装する` (ブランチの `47967d8` と同題・同内容) |
| ブランチの他の主要コミット (`35f0f3c` SHaRC の方式別分離、`5b98c9b` FrameResult、`644c753` API 26) | main に同題のコミットがある (`851c1f9`、`1e9f84f`、`92ba700`) |
| ブランチ末尾 `847b56c` (const ラムダ修正) | main の `vulkan_context.cpp` に修正後の形があり、`spec/plan/problem_logs/2026-09-11-native-recovery-const-lambda.md` も main にある |
| `git diff main feat/native-metal-dx12-recovery --stat` | 288 files, +2086 / −16156。ブランチ側が main の新しい内容 (`context_init_result.h`、`frame_gate.h`、`vulkan_capability_check` 等) を**持っていない**向きの差が大半 |

→ ブランチは main へ別コミットとして取り込み済みで、今はブランチの方が古い。全体マージは main の
新しい内容を消す方向に働くので行わない。詳細は §3.8。

---

## 2. 比較した案

### 2.1 境界をどこに置くか

| 案 | 内容 | 消費側が方式を知らないか | hot path | 判断 |
|---|---|---|---|---|
| A. RHI を公開する | `IRhiDevice` / `IRhiCommandEncoder` を消費側が使う (Ergo の `IGpuBackend` 型) | 型は隠せるが、能力差 (compute が無い等) の判断が消費側に出る | 消費側の記録が全部仮想呼び出し | 不採用 |
| B. 方式ごとの完全別実装を並べる | 今の `pictor_webgl` と本体の関係を 5 方式に広げる | 消費側は方式ごとに別 API | 速い | 不採用 (二重実装。neco 指示と逆) |
| **C. 公開面 + 内部 RHI の 2 層** | 消費側は `pictor/gfx/` (方式中立の資源・パス・記録) だけを使い、RHI は内部 | 守れる。降格も Pictor 内で完結 | パス単位の仮想 1 回 + 方式内の直値ループ | **採用** |

### 2.2 方式の選択時期

| 案 | 内容 | 判断 |
|---|---|---|
| コンパイル時のみ (1 ビルド 1 方式) | テンプレート / `#if` で方式を確定 | Web で WebGPU → WebGL2 の実行時切替ができない。不採用 |
| 実行時のみ (全方式を常にリンク) | 動的ロード | 依存 SDK が全部必要になり PC-RULE-002 に反する。不採用 |
| **ビルドで同梱、実行時に選択** | CMake で方式ごとに ON/OFF、同梱分から実行時に選ぶ | **採用**。wasm は WebGPU + WebGL2 を 1 本に同梱、Windows は Vulkan + DX12 |

### 2.3 シェーダの正本

| 案 | 移行量 | 全方式への出力 | 判断 |
|---|---|---|---|
| HLSL 正本 + DXC (既存 dx12 設計の推奨) | GLSL 約 84 本を書き直し | DXIL / SPIR-V は直接、MSL / WGSL / GLSL ES は SPIR-V 経由 | 書き直しの割に WGSL / GLSL ES は結局 SPIR-V 経由 |
| Slang 正本 | 全面書き直し | 5 出力すべて自前 | 将来性は高いが、今の資産と移行量が釣り合わない |
| **GLSL 継続 + SPIR-V を中間に固定** | 0 本 (規約違反の手直しのみ) | SPIR-V → Tint (WGSL) / SPIRV-Cross (GLSL ES・MSL・HLSL) → DXC (DXIL) | **採用**。Slang / HLSL は SPIR-V を出す入口として後で併存できる |

### 2.4 WebGPU の実装

| 案 | 判断 |
|---|---|
| Emscripten 旧 `-sUSE_WEBGPU` | 廃止方向。不採用 |
| **`webgpu.h` (標準 C ヘッダ) に書く。Web は emdawnwebgpu、ネイティブは任意で Dawn** | **採用**。Web と開発機で同じ実装系統を使え、ネイティブで WebGPU 方式を試せる |
| wgpu-native | `webgpu.h` 準拠だが Dawn との差が残る。ネイティブ検証用としても Dawn と揃えた方が差分が少ない。不採用 (将来の選択肢として残す) |

---

## 3. 推奨案の詳細

### 3.1 RHI の境界 (設問 1)

#### 層の構成

```
消費側 (Ergo / Ludellus-Native / 非ゲーム host)
   │  pictor/gfx/*.h  … 方式の型・名前を含まない公開面 (§3.9)
   ▼
RenderSystem (公開)    … 方式の選択、BackendReport、資源ハンドル、フレームの開始/終了
   │
FrameGraph 実行 (内部) … CompiledGraph (方式中立) + DrawStream (方式中立の flat 配列)
   │  IRhiDevice::execute(const CompiledGraph&, const FramePacket&)  ← 仮想はフレームに 1 回
   ▼
src/rhi/{vulkan,webgpu,webgl2,d3d12,metal}/  … 方式ごとの直値ループ、方式の型はここだけ
```

#### 内部 RHI のインターフェース (責務別に分ける)

| インターフェース | 責務 | 呼ばれる頻度 |
|---|---|---|
| `IRhiDevice` | 生成・破棄、能力照会 (`DeviceCaps`)、資源とパイプラインの生成、`compile_graph` / `execute` | 初期化・資源生成・フレーム 1 回 |
| `IRhiSwapchain` | acquire / present / resize、`FrameResult` を返す | フレーム 1〜2 回 |
| `IRhiResourceTable` | ハンドル (`BufferId` / `TextureId` / `SamplerId` / `PipelineId` / `BindGroupId`) → 方式の実体。世代付き 32bit 添字 | 生成・破棄時のみ。hot path は添字直引き |
| `IRhiUploader` | staging / UMA 直書き / WebGPU `writeBuffer` / WebGL2 `bufferSubData` の差を吸収 | フレーム 1 回にまとめて |
| `IRhiQueryPool` | timestamp / occlusion。`GpuTimerManager` をこの上に載せる | フレーム 1 回の解決 |
| `IRhiCommandRecorder` (方式内部の具象) | 1 パス内の記録。**仮想にしない**。各方式の `execute` 内で具象型として使う | draw 単位 (直値) |

太い `IRenderer` は作らない (CLAUDE.md の I 原則)。`IRhiCommandEncoder` を仮想関数で draw 単位に
呼ぶ案 (既存 dx12 設計 §3) は hot path に仮想呼び出しを入れるので採らず、記録の単位を「パス列」に上げる。

#### hot path の不変条件との両立

- CompiledGraph / DrawStream は **POD の flat 配列**で、string / map / 方式ハンドルを含まない (§3.5)。
- 方式は `compile_graph()` で、CompiledGraph と同じ並びの**並列配列** (`VkRenderPass[]`、
  `ID3D12PipelineState*[]`、`WGPURenderPipeline[]` …) を自分の中に作る。`execute()` は添字で両者を引くだけ。
- 仮想呼び出しは `IRhiDevice::execute()` のフレーム 1 回と、消費側の独自パス記録のコールバック (パス 1 回)。
  draw / bind 単位の仮想呼び出しは 0。
- 方式の具象型は `final` にし、`execute()` 内は具象型で書く (脱仮想化に頼らない)。

#### 公開面に出すもの / 内側に閉じるもの

| 公開 (`pictor/gfx/`) | 内側 (`src/rhi/`、公開ヘッダに出さない) |
|---|---|
| `RenderSystem`、`RenderSystemConfig` (方式の希望順・上書き) | `IRhiDevice` ほか RHI 一式 |
| `BackendReport` (選ばれた方式・Tier・降格一覧)、`DegradationEntry` | 方式ごとの device / queue / swapchain / heap |
| 資源の記述子 (`BufferDesc` / `TextureDesc` / `SamplerDesc`) と世代付きハンドル | ハンドル → 方式実体の表 |
| `GfxFormat` (方式中立の形式列挙)、`LoadOp` / `StoreOp`、`ShaderStageMask` | `VkFormat` / `DXGI_FORMAT` / `MTLPixelFormat` / `WGPUTextureFormat` / GL enum への変換 |
| `ShaderBundleRef` (シェーダ束の参照名) | 方式ごとのバイナリ選択・モジュール生成 |
| `PassRecorder` (独自パス用の方式中立コマンド列。§3.5) | コマンド列の再生 |
| `FrameResult` / `FrameStatus` / `ContextInitResult` (既存。方式中立) | VkResult / HRESULT / WebGPU device lost 等の変換 |
| `ISurfaceProvider` / `NativeWindowHandle` (既存 + Web canvas を追加) | `VkSurfaceKHR` / `IDXGISwapChain` / `CAMetalLayer` / `GPUCanvasContext` / WebGL2 context |
| `DeviceCaps` (読むだけ。分岐に使わせる目的ではなく表示・記録用) | 能力の検出処理 |

移行期間だけ、`pictor/backend/vulkan_interop.h` などの**任意の相互運用ヘッダ**に native ハンドルの取り出しを置く
(§3.9)。これは「方式を知らない」公開面には数えず、検査 (§4 S0) の許可リストで別扱いにする。

### 3.2 5 方式の能力差の扱い (設問 2)

#### 能力の型

```cpp
namespace pictor::gfx {
enum class CapabilityTier : uint8_t { A_Raster, B_Compute, C_Full };
enum class Feature : uint16_t {
    Compute, StorageBufferVS, StorageBufferFS, StorageTexture,
    IndirectDraw, IndirectDrawCount, MultiDrawIndirect, FirstInstance,
    TimestampQuery, TimestampInsidePass, OcclusionQuery,
    Bindless, Msaa4x, Msaa8x, ExplicitRenderPassMerge /* subpass / tile */,
    FloatRenderTarget, Bc, Etc2, Astc, DepthClamp, ... , Count_
};
struct DeviceCaps {
    CapabilityTier tier;
    std::bitset<size_t(Feature::Count_)> features;
    DeviceLimits limits;   // max bind groups, max storage buffer size, max color attachments, ...
};
}
```

#### 方式ごとの既定 (実装前の想定。各方式で実測照会し、表は既定値の目安)

| 能力 | Vulkan | D3D12 | Metal | WebGPU | WebGL2 |
|---|---|---|---|---|---|
| Tier | C | C | C | B | A |
| compute | ○ | ○ | ○ | ○ | × |
| storage buffer (VS/FS) | ○ | ○ | ○ | ○ (VS の書き込みは ×) | × (UBO / テクスチャで代替) |
| indirect draw | ○ | ○ (ExecuteIndirect) | ○ | ○ (count なし) | × |
| indirect draw count | 1.2 / 拡張で ○ | ○ | △ (ICB で代替) | × | × |
| first instance | ○ | ○ | ○ | 拡張 (`indirect-first-instance`) | × (`gl_InstanceID` は 0 起点) |
| timestamp | ○ | ○ | △ (counter sample、端末差) | 拡張 (`timestamp-query`)、精度は丸め | `EXT_disjoint_timer_query_webgl2` (ほぼ無い) |
| bindless | 拡張で ○ | ○ (SM6.6) | ○ (argument buffer tier 2) | × | × |
| MSAA | ○ | ○ | ○ | 4x のみ | ○ (renderbuffer) |
| render pass | 明示 (RP / dynamic rendering) | 暗黙寄り (`BeginRenderPass` は任意) | 明示 (descriptor) | 明示 (`beginRenderPass`) | 暗黙 (FBO bind + clear + invalidate) |

#### 降格規則

1. **能力要求は宣言で持つ。** パス (`RenderPassDef`)・機能モジュール (SHaRC / GPU-driven / SSAO …)・
   Visus の variant が `requires: ["Compute", "StorageBufferVS"]` と `fallback` を持つ。
   `fallback` は「別のパス / モジュール / variant の ID」か「`omit` (省略して記録する)」。
2. **解決はグラフのコンパイル時に 1 回。** `PipelineCompiler` がアクティブな preset と `DeviceCaps` を突き合わせ、
   満たせないものを fallback へ置き換える。fallback も満たせなければ再帰的に辿り、最後まで満たせなければ
   **コンパイル失敗** (`ContextInitStatus::MissingCapability` と同じ語彙のエラー) とする。黙って空のパスにしない (PC-RULE-002)。
3. **`omit` は宣言した場合だけ許す。** たとえば timestamp が無い方式で GPU timer を `omit` するのは可。
   SSAO を `omit` するかは preset が宣言する。宣言が無ければ失敗。
4. **結果は `BackendReport::degradations` に積む。** 各要素は `{feature, subject_id, action (Replaced/Omitted/Emulated), replacement_id, reason}`。
   - stderr / ログに 1 回出す。
   - FrameTap (`spec/feature/frame-tap.md`) の初回行に `backend` / `degradations` を載せる。
   - 統計オーバーレイの隅に「WebGL2 予備描画: 3 件の降格」の形で出す (オーバーレイを無効にした host でも
     `RenderSystem::report()` で読める)。
5. **Emulated (下回りで実装を変えるが見た目は同じ) も記録する。** 例: push constant → 動的オフセット UBO、
   indirect draw count → CPU 側で count を確定して通常 indirect、WebGL2 の first instance → instance 添字を UBO で補正。
   性能だけが変わる降格も「黙って」にしない。
6. **消費側は降格を書かない。** 消費側が書くのは素材 (variant) と「省略してよいか」の宣言だけ。方式名での分岐は API として提供しない。

#### 代表的な降格の例

| 機能 | Tier C | Tier B (WebGPU) | Tier A (WebGL2) |
|---|---|---|---|
| GPU-driven cull / LOD | compute + indirect count | compute + indirect (count は compute で 0 埋めした固定上限) | CPU cull (`CullingSystem`) + 通常 instanced draw に Replaced |
| SHaRC / compute GI | そのまま | そのまま (storage 制限内) | preset 宣言に従い bake 済み probe へ Replaced、無ければ失敗 |
| SSAO (compute 版) | compute | compute | fragment 版へ Replaced、または preset が `omit` を宣言 |
| GPU timer | timestamp | 拡張があれば timestamp、無ければ Omitted | Omitted |
| SDF アバター (Ludellus) | compute | compute | Visus variant の三角形アバターへ Replaced |
| skinning | compute または VS | 同左 | VS (ボーン行列は UBO / テクスチャ) |

### 3.3 バインディングモデルの共通化 (設問 3)

#### Pictor の資源宣言

- **バインドグループは最大 4 つ、更新頻度で固定の役割を持つ** (WebGPU の既定上限 `maxBindGroups = 4` に合わせる)。
  - group 0: フレーム (カメラ・光源・時間)
  - group 1: パス (入力 attachment・影マップ・パス定数)
  - group 2: マテリアル (テクスチャ・マテリアル定数)
  - group 3: 描画 (オブジェクト定数。動的オフセット 1 本)
- 各 binding は `{slot, kind (Uniform/StorageRO/StorageRW/SampledTexture/Sampler/StorageTexture), stages, array_count}`。
- **テクスチャとサンプラは分けて宣言する** (combined image sampler を正本に使わない)。WebGL2 / GLSL ES 向けの
  結合は変換時に機械的に行う (§3.4)。
- **push constant は正本に使わない。** 小さな定数は group 3 の動的オフセット UBO に置く。既存シェーダの push constant は
  S2 で group 3 へ移す (Vulkan 方式でも同じ形にして、方式差を作らない)。
- 宣言の出所はシェーダの反射 (SPIR-V reflection) で、手書きの layout 定義は持たない。反射結果はシェーダ束に同梱する (§3.4)。

#### 方式ごとの対応

| Pictor | Vulkan | D3D12 | Metal | WebGPU | WebGL2 |
|---|---|---|---|---|---|
| BindGroupLayout | `VkDescriptorSetLayout` | root signature の descriptor table 1 つ (CBV/SRV/UAV) + sampler table 1 つ | 引数添字の範囲 (初期は直接 `set*Buffer:atIndex:`、後で argument buffer) | `WGPUBindGroupLayout` | 平坦化した UBO binding point / texture unit の範囲 |
| PipelineLayout (4 グループ) | `VkPipelineLayout` | root signature (group 3 の動的 UBO は root CBV) | 添字表 (group×slot → buffer/texture index) | `WGPUPipelineLayout` | program ごとの binding 再割当表 |
| BindGroup | `VkDescriptorSet` | shader-visible heap の連続領域 | 束ねた参照列 (argument buffer 化後は 1 buffer) | `WGPUBindGroup` | (UBO 範囲, texture, sampler) の配列 |
| 動的オフセット | dynamic UBO | root CBV の GPU VA | `setBufferOffset` | dynamic offset | `bindBufferRange` |
| Storage buffer | SSBO | SRV/UAV (Structured / ByteAddress) | `device` buffer | storage | × (Tier A: 降格) |

- WebGL2 の再割当表は、シェーダ束の生成時に (group, slot) → (UBO binding / texture unit) を決めて同梱する。
  実行時に名前で `glGetUniformBlockIndex` を引くのは初期化時 1 回だけ。
- D3D12 の root signature はパイプラインレイアウトごとに 1 つを生成時に作り、hot path では
  `SetGraphicsRootDescriptorTable` を添字で呼ぶ。
- bindless は共通モデルに入れない。Tier C 専用の任意機能 (`Feature::Bindless`) として、必要なモジュールだけが
  `requires` で要求し、Tier B / A では fallback を宣言する。

### 3.4 シェーダ戦略 (設問 4)

#### 経路

```
GLSL 450 (正本、今の shaders/*.vert|frag|comp)      ─┐
(将来) Slang / HLSL  ── slangc / dxc -spirv ─────────┤
                                                     ▼
                                    SPIR-V (中間・Vulkan 方式はそのまま使う)
                         ┌──────────────┬──────────────┬───────────────┐
                    Tint (WGSL)   SPIRV-Cross     SPIRV-Cross      SPIRV-Cross (HLSL 6.x)
                         │         (GLSL ES 3.00)   (MSL 2.x)            │ DXC
                      WebGPU         WebGL2          Metal               DXIL → D3D12
```

- **ビルド時に生成する。** `pictor_shaderc` (新しいビルド時ツール、ホスト PC で動く) が 1 ソースから全方式分を作り、
  反射結果 (binding 宣言・頂点入力・workgroup size) と一緒に **シェーダ束** (`<name>.pshader`、方式ごとの blob + 反射 JSON) にまとめる。
- **Web では実行時コンパイルをしない。** wasm にシェーダコンパイラを入れない (大きさと起動時間)。
- **実行時コンパイルはデスクトップの開発用ホットリロードだけ**の任意モジュールにする (`PICTOR_SHADER_HOT_COMPILE`)。
  既存の SHaRC DX12 の `D3DCompile` 実行時コンパイルは S6 で束からの読み込みに置き換える。
- 束に入れる方式は、そのビルドで ON の方式だけ (CMake が `pictor_shaderc --targets=...` に渡す)。

#### 変換を通すためのシェーダ規約 (`pictor_shaderc` が lint として検査)

- set 番号は 0..3 を §3.3 の役割で使う。push constant は使わない (S2 で移行)。
- テクスチャとサンプラは分ける (`texture2D` + `sampler`)。
- storage buffer に `vec3` を置かない (std430 と MSL / WGSL の配置差。既存 SHaRC は既に守っている)。
- WebGL2 で使うシェーダは storage buffer / image / compute を使わない。使う場合は反射結果から「Tier B 以上」と
  自動判定し、束のメタデータに載せる (降格判定 §3.2 の入力)。
- `gl_DrawID` 等、Tier A に無い組み込み変数も同様に自動判定する。

#### Visus shader package との関係

- `VisusShaderRef` (builtin: / stages / visus:) の解決先を「ソースファイル」から「シェーダ束」に変える。
  Visus 側の記述 (参照名・params・metadata) は変えない。
- `VisusShaderPackage::metadata` の `shader.vertex_layout` 等は束の反射結果と突き合わせ、食い違いは読み込み時の明示エラーにする。
- Visus の variant に `requires` を足し (§3.2)、方式の能力で選ばれる。

#### 消費側のカスタムシェーダ

- 消費側は CMake 関数 `pictor_add_shader_bundle(<target> SOURCES ... )` で自分の GLSL を束にする。
  Pictor と同じツール・同じ規約で、1 ソースから全方式へ出る。
- 実行時は `ShaderBundleRef{"my_game/avatar_sdf"}` で参照する。方式ごとのファイルを消費側が選ぶ API は無い。

#### 既存資産の移行量 (この作業場所の集計)

| 場所 | 本数 | 移行 |
|---|---|---|
| `shaders/*.{vert,frag,comp,glsl}` | 27 | push constant と combined sampler の手直しのみ |
| `shaders/postprocess/` | 27 | 同上 |
| `shaders/sharc/` | 19 (+ `hlsl/` 8) | GLSL を正本に揃え、`hlsl/` は DXIL 生成が安定したら削除 (それまで方式別の上書きとして残す) |
| `shaders/ui/`、`decal/`、`xr/` | 7 | 手直しのみ |
| `shaders/webgl/` | 4 | 束へ移した後、GLSL ES 手書きは廃止 (正本の GLSL 450 から生成) |

`#extension` は `GL_GOOGLE_include_directive` のみで、bindless / subgroup 等の変換しにくい拡張は使っていない。

### 3.5 パイプライン系の RHI 化 (設問 5)

#### CompiledPass から Vk 型を外す

```cpp
struct alignas(64) CompiledPass {          // 方式中立、POD、≤ 128B を目標
    uint16_t target_set;                   // RenderTargetSetId (attachment の組)
    uint16_t pipeline;                     // PipelineId (0xFFFF = pass_type の既定)
    uint16_t bind_group_pass;              // group 1 の BindGroupId
    uint16_t pass_id;                      // RenderPassRegistry の添字 (既存と同義)
    uint16_t first_transition, transition_count;  // TransitionOp 配列の範囲
    uint16_t timestamp_begin, timestamp_end;
    int32_t  area[4];                      // render area (x, y, w, h)
    uint32_t filter_mask;
    uint8_t  sort_mode, pass_type, clear_count, flags;
    uint8_t  load_ops[8], store_ops[8];    // LoadOp / StoreOp
    // clear 値は CompiledGraph::clear_values (float[4] の flat 配列) の範囲で引く
    const char* debug_name;                // hot path では読まない
};
```

- `VkRenderPass` / `VkFramebuffer` / `VkDescriptorSet` / `VkPipeline` / `VkClearValue` は
  Vulkan 方式の `VkGraphTables` (CompiledPass と同じ並びの並列配列) へ移す。
- `RenderTargetSet` = color 最大 8 + depth 1 の `TextureId` と形式。Vulkan 方式は compile 時に
  `VkRenderPass` / `VkFramebuffer` を (形式・load/store の組) のキーで作り、hot path では添字で引く。
  dynamic rendering が使える端末ではそれを使うが、Android の古い端末向けに `VkRenderPass` 経路を既定に残す。
- swapchain 対象のパスは `target_set` が「swapchain 画像」を指し、方式が acquire 済み画像へ差し替える
  (既存の `framebuffers[4]` + `is_swapchain` の分岐を、方式内の 1 分岐へ移す)。
- **バリア / 状態遷移はグラフが計算する。** パスの読み書き集合から `TransitionOp{texture/buffer, from, to}` を
  コンパイル時に列挙し、Vulkan / D3D12 はバリアに変換、Metal / WebGPU / WebGL2 はほぼ無処理 (必要な所だけ encoder の区切り)。

#### DrawStream (バッチの記録)

- `BatchBuilder` の `RenderBatch` を、方式中立の SoA (`pipeline[]`、`bind_group_material[]`、`object_offset[]`、
  `vertex_buffer[]`、`index_buffer[]`、`index_count[]`、`instance_count[]`、`first_index[]`、`first_instance[]`) に詰める。
- `CompiledBatchRecorder` は DrawStream を作る側に変わり、各方式の `execute` が DrawStream を直値ループで再生する。
  実測 draw 数 / 三角形数の集計は再生側で行う (既存の D-2 是正を維持)。

#### host_recorded パスと post-process

- **post-process はグラフの通常パスに展開する。** 今は `PostProcessPipeline` が内部に複数のサブパスを持ち、
  `FLAG_IS_HOST_RECORDED` で 1 エントリとして扱っている。これを preset の展開時に N 個の CompiledPass に開き、
  方式が普通に記録する。ホストが `initialize_vulkan` / `record` を呼ぶ形は撤去する。
- **host_recorded は「方式中立の独自パス」に置き換える。** 消費側は `PassRecordFn` の代わりに
  `void(PassRecorder&, const PassContext&)` を登録する。`PassRecorder` は POD のコマンド列
  (`SetPipeline` / `SetBindGroup` / `SetVertexBuffer` / `Draw` / `DrawIndexed` / `Dispatch` …) を
  flat 配列へ追記するだけで、仮想呼び出しを持たない。各方式がその列を再生する。
  - 利点: 方式を知らずに独自描画が書ける / headless で記録内容を検査できる / WebGL2 でも同じ。
  - `Dispatch` を Tier A で記録した場合、パスが `requires: Compute` を宣言していなければコンパイル時、
    宣言漏れで実行時に来た場合は記録時の明示エラーにする (黙って捨てない)。
- 移行期間は `VkCommandBuffer` を受ける旧 `PassRecordFn` を `pictor/backend/vulkan_interop.h` 側に残し、
  Vulkan 方式でだけ使える互換経路にする (S8 で撤去)。

### 3.6 ウィンドウ / 表示面 (設問 6)

#### surface_provider と各方式のスワップチェーン

| provider / handle | Vulkan | D3D12 | Metal | WebGPU | WebGL2 |
|---|---|---|---|---|---|
| GLFW (Win32) | `vkCreateWin32SurfaceKHR` | `IDXGIFactory::CreateSwapChainForHwnd` (flip model) | — | Dawn: `WGPUSurfaceSourceWindowsHWND` (開発用) | — |
| GLFW (X11 / Wayland) | xlib / wayland surface | — | — | Dawn (開発用) | — |
| GLFW (Cocoa) | MoltenVK (`VK_EXT_metal_surface`) | — | `CAMetalLayer` (S7 以降で macOS も) | Dawn (開発用) | — |
| Android `ANativeWindow` | `vkCreateAndroidSurfaceKHR` | — | — | — | — |
| iOS `CAMetalLayer` | (使わない) | — | `nextDrawable` | — | — |
| **Web canvas (新設)** | — | — | — | `WGPUSurfaceSourceCanvasHTMLSelector` → `GPUCanvasContext` | `emscripten_webgl_create_context` |

- `NativeWindowHandle::Type` に `WebCanvas` (CSS セレクタ文字列) を足す。
- `ISurfaceProvider::get_required_instance_extensions()` は Vulkan 固有なので、Vulkan 方式が handle の種類から
  自分で導くように移し、`ISurfaceProvider` からは段階的に外す (S8)。
- 既存 `Dx12Context` / `MetalContext` の swapchain 部分は `IRhiSwapchain` の D3D12 / Metal 実装へ移す (§3.8)。
  `NativeDrawCallback` (`void* encoder` を渡す口) は方式を消費側に漏らすので撤去する。

#### FrameResult による復旧を方式共通にする

既存 `FrameStatus` (`Ready` / `RecreateSwapchain` / `SurfaceLost` / `DeviceLost` / `Suspended` …) をそのまま
全方式の返り値にする。各方式の事象の対応:

| FrameStatus | Vulkan | D3D12 | Metal | WebGPU | WebGL2 |
|---|---|---|---|---|---|
| RecreateSwapchain | `OUT_OF_DATE` / `SUBOPTIMAL` | `ResizeBuffers` 要求、occluded | drawable サイズ変化 | canvas サイズ変化 → `configure` | canvas サイズ変化 |
| SurfaceLost | `SURFACE_LOST_KHR` | HWND 消失 | layer 消失 | canvas 切り離し | — |
| DeviceLost | `ERROR_DEVICE_LOST` | `DXGI_ERROR_DEVICE_REMOVED/RESET` | command buffer エラー / デバイス取り外し通知 (macOS) | `device.lost` | `webglcontextlost` |
| Suspended | lifecycle 由来 (方式非依存。既存どおり) | 同 | 同 | タブ非表示 (rAF 停止) | 同 |

- **DeviceLost からの復旧を Pictor の中で完結させる。** `RenderSystem` が device を作り直し、資源表のハンドルを
  保ったまま実体を作り直す。消費側のハンドルは無効にならない。
  - 作り直しの素材: Pictor のローダが読んだ資源は「出所 (パス / 束の名前)」を資源表に記録して読み直す。
    消費側が CPU から渡した資源は既定で CPU 側の写しを持たず、`ResourceLost` 通知 (ハンドル単位) を出して
    次のフレームで再アップロードしてもらう。写しを Pictor に持たせる設定 (`retain_cpu_copy`) も選べる (§5 未決 U-4)。
- 復旧手順 (`portability/mobile-surface-recovery.md` §5) は方式共通の手順として RenderSystem へ移す。

#### 方式の自動選択規則

| プラットフォーム | 既定の希望順 | 理由 |
|---|---|---|
| Windows | Vulkan → D3D12 | Vulkan が成熟した経路。D3D12 は Vulkan 初期化失敗時の予備から始め、D3D12 が全 preset 対応になった時点で順序を見直す (§5 未決 U-1) |
| Linux | Vulkan | — |
| macOS | Vulkan (MoltenVK) → Metal | Metal 方式が全 preset 対応するまで。対応後は Metal を先頭に (U-1) |
| iOS | Metal | MoltenVK を使わない既決方針 (`portability/mobile.md` 2026-09-10) |
| Android | Vulkan | GLES 予備は作らない (§9.3) |
| Web | WebGPU → WebGL2 | neco 指示 |

- 上書き: `RenderSystemConfig::backend_preference` (配列)、環境変数 `PICTOR_BACKEND` (開発用)、
  Web は host が URL 引数などから同じ設定へ渡す。
- **上書きした方式が使えないときは明示エラー**。自動で次へ進めるのは既定の希望順のときだけ
  (`allow_fallback` で上書き時の自動切替を許可できる)。
- 選んだ方式・試して失敗した方式とその理由 (`ContextInitResult`) を `BackendReport` に残す。

### 3.7 WebGPU / WebGL2 の実装形態 (設問 7)

#### WebGPU

- コードは標準 `webgpu.h` (`wgpu*` C API) に書く。
- Web: Emscripten の `emdawnwebgpu` port (`--use-port=emdawnwebgpu`)。旧 `-sUSE_WEBGPU` は使わない。
- ネイティブ: 任意で Dawn (`PICTOR_RHI_WEBGPU_NATIVE=ON`、Dawn は利用者が用意した prebuilt を
  `PICTOR_DAWN_DIR` で指す。暗黙 fetch しない)。目的はブラウザ無しで WebGPU 方式を開発・CI ビルドすること。
- **初期化は非同期を前提にする。** `requestAdapter` / `requestDevice` は Web で非同期。ASYNCIFY / JSPI に頼らず、
  `RenderSystem::initialize()` が `ContextInitStatus::Pending` (新設) を返し、host のフレームループ (rAF) の中で
  `poll_initialize()` が完了するまで描画しない状態機械にする。
- WGSL は束から読む (§3.4)。`timestamp-query` / `indirect-first-instance` / `float32-filterable` 等の
  optional feature は要求して、得られたものを `DeviceCaps` に反映する。

#### WebGL2

- **既存 `pictor_webgl` は捨てずに方式の内側へ移す。** `webgl_context` / `webgl_shader` / `webgl_buffer` は
  WebGL2 方式の下回り (`src/rhi/webgl2/`) として再利用する。
- `WebGLRenderer` (icosphere の参考) と `WebMeshRenderer` (JS から使う web-mesh モジュール) は、
  公開 API (`web-mesh/` の ES module 契約) を保ったまま、内部を `RenderSystem` 経由へ載せ替える (S4)。
  載せ替えまでは今の実装を並置で残す。
- Tier A なので、compute を要るパスはコンパイル時に降格される (§3.2)。WebGL2 方式に compute を
  エミュレートする仕組み (transform feedback 等) は作らない。

#### Ludellus-Native の wasm 化に必要な最低条件

1. Pictor 本体 (scene / culling / batch / text / animation / pipeline profile) が Vulkan 無しで Emscripten ビルドできる
   (`pictor_core` の切り出し。`portability/web.md` Stage 1 と同じ作業。`pictor.h` から Vulkan 系 include を外す)。
2. シングルスレッドの job dispatcher (既存 `IJobDispatcher` に `SingleThreadDispatcher`)。COOP/COEP を要求しない。
3. WebGPU 方式と WebGL2 方式が、Ludellus-Native 段階 2 が使うパス (不透明メッシュ + skinning、影、
   UI / 文字オーバーレイ、post 最小構成) を描ける。SDF アバターは WebGPU で compute、WebGL2 で variant へ降格。
4. シェーダ束が両方式分を含む (ビルド時生成)。
5. 非同期初期化 (上記) と rAF のフレームループ。
6. 資産は fetch → MEMFS (既存方針どおり。Pictor のローダは同期 file I/O のまま通す)。
7. `BackendReport` を host が画面隅に出せる (Ludellus 側の「落とした事実を画面と記録に出す」に対応)。

### 3.8 DX12 / Metal の既存実装の扱い (設問 8)

**推奨: ブランチは取り込まない。main にある実装を RHI の D3D12 / Metal 方式の素体として移す。**

根拠 (§1.3 の事実):

- DX12 / Metal の描画面・復旧・SHaRC DX12 は main に同じ内容で入っている。ブランチから移す追加分が無い。
- ブランチと main の差は、ブランチ側が main の新しい surface 系 (`context_init_result.h`、`frame_gate.h`、
  `vulkan_capability_check`、`vulkan_portability_probe`) を持たない向きが大半で、全体マージは退行する。
- よって「全体マージ / 必要部分の移植 / 作り直し」の三択では「**main 上の既存コードからの移設**」になる
  (ブランチは閉じてよい。閉じる判断は人間側。§5 U-6)。

移し方:

| 既存 (main) | 移設先 | 扱い |
|---|---|---|
| `Dx12Context` (`dx12_context.cpp` 392 行: swapchain / clear / mesh draw / recover) | `src/rhi/d3d12/` の device・swapchain・uploader | swapchain と復旧は移設。mesh draw は DrawStream 再生に置き換え |
| `MetalContext` (`metal_context.mm` 208 行) | `src/rhi/metal/` | 同上 |
| `NativeRenderConfig` / `NativeMeshView` / `NativeDrawCallback` | 撤去 | 方式中立の資源 API と `PassRecorder` に置き換え。互換期間は §3.9 |
| `SharcDx12Executor` (927 行) / `SharcGpuExecutor` (Vulkan) | SHaRC モジュールの RHI 版 1 本 | SHaRC を RHI の compute で書き直した時点で方式別 executor を削除。それまでは方式別モジュールとして残す (`851c1f9` の分離を維持) |
| SHaRC の `hlsl/` 8 本 + `D3DCompile` 実行時コンパイル | シェーダ束 | S6 で置き換え |

### 3.9 公開 API と消費側 (設問 9)

#### PictorRenderer

- `compile_render_graph(VkDevice, ...)` / `render_compiled(VkCommandBuffer, ...)` を
  `RenderSystem` 経由の `begin_frame()` → `render()` → `end_frame()` に統合する。registries / device の受け渡しは不要になる。
- 旧シグネチャは `#ifdef PICTOR_HAS_VULKAN` のまま `pictor/backend/vulkan_interop.h` へ移し、
  `[[deprecated]]` を付けて互換期間を置く。

#### c_api.h

- `pictor_create_renderer_webgl` / `pictor_create_renderer_vulkan` (後者は未実装で常に失敗する) を、
  方式を含まない `pictor_create_renderer(const PictorSurfaceDesc*, const PictorBackendPreference*)` に置き換える。
- `pictor_backend_report()` (選ばれた方式・降格数・各降格の文字列) を足す。
- 旧 2 関数は新関数への薄い転送として互換期間残す。`pictor_c_api_version()` を上げる。

#### Ergo `ergo_render`

Pictor から Ergo への依存は作らない (AGENTS.md)。移行は Ergo 側の作業で、Pictor は移行先の公開面を用意するだけ。

| Ergo 今 (Vk 型を公開) | 移行先 (Pictor 公開面) |
|---|---|
| `FrameComposer::add_pass(VkRenderPass, std::vector<VkClearValue>)` | Pictor の preset / `RenderPassDef` (load/store・clear は方式中立) |
| framebuffer provider `VkFramebuffer(*)(uint32_t, void*)` | 不要 (swapchain 対象は Pictor が解決) |
| hook `void(*)(VkCommandBuffer, uint32_t, const FrameContext&, void*)` | 独自パス `void(PassRecorder&, const PassContext&)` |
| `IRenderLayer::set_render_pass(VkRenderPass)` / `record(VkCommandBuffer, VkExtent2D)` | `IRenderLayer::record(PassRecorder&, const PassContext&)`。パイプライン生成は `PipelineDesc` + `ShaderBundleRef` |
| `FrameContext::extent` (`VkExtent2D`) | `uint32_t width, height` |
| `render_backend.h` の `RenderPlatform` / `VulkanBackendUnavailable` | Pictor の `BackendReport` を読む形へ |
| `gpu_particle/IGpuBackend` (SPIR-V 入力) | Pictor の compute (`PassRecorder::Dispatch` + シェーダ束)。Ergo の host 側実装を Pictor の上に書けば WebGPU も同時に得られる |

- 互換期間: **あり**。S1〜S7 の間は旧 Vulkan 経路 (interop ヘッダ) と新公開面が両立し、Ergo / Ludellus-Native は
  自分の段階で移る。S8 (撤去) は主要消費側 (Ergo、Ludellus-Native) が移行済みであることを条件にする。

### 3.10 ビルド構成 (設問 10)

#### CMake オプション

| オプション | 既定 | 備考 |
|---|---|---|
| `PICTOR_RHI_VULKAN` | デスクトップ / Android で ON (Vulkan 発見時)、iOS / Web で OFF | 今の `Vulkan_FOUND` 判定を引き継ぐ |
| `PICTOR_RHI_D3D12` | Windows で ON | 今の `if(WIN32)` を option 化。Windows SDK 同梱なので追加依存なし |
| `PICTOR_RHI_METAL` | Apple で ON (iOS は必須) | macOS は S7 から |
| `PICTOR_RHI_WEBGPU` | Emscripten で ON、ネイティブ OFF | ネイティブで ON にするには `PICTOR_DAWN_DIR` 必須 (無ければ configure 時に明示エラー) |
| `PICTOR_RHI_WEBGL2` | Emscripten で ON、ネイティブでは選べない | 既存 `PICTOR_BUILD_WEBGL` を吸収 |
| `PICTOR_SHADER_TARGETS` | ON の方式から自動 | `pictor_shaderc` に渡す |
| `PICTOR_SHADER_HOT_COMPILE` | OFF | 開発用の実行時コンパイル (shaderc / Tint / SPIRV-Cross をリンク) |

- ON の方式が 0 個なら configure 時に明示エラー (headless テスト用の `PICTOR_RHI_NULL` は別に用意する。§4 S0)。
- 公開する compile definition は `PICTOR_HAS_<方式>` をやめ、**公開ヘッダは方式マクロに依存しない**ようにする
  (方式マクロは PRIVATE。interop ヘッダだけが例外)。

#### 依存 SDK

既存方針 (取り込まない / 暗黙 fetch しない) を維持する。

| 依存 | 扱い |
|---|---|
| Vulkan SDK / NDK | 既存どおり発見 |
| Windows SDK (D3D12 / DXGI) | 既存どおり |
| DXC (DXIL 生成) | ビルド時ツール。発見できなければ D3D12 のシェーダ束が作れないので configure 時に明示エラー (D3D12 を OFF にする案内付き) |
| SPIRV-Cross / Tint / glslang (glslc) | ビルド時ツール。`pictor_shaderc` が呼ぶ。発見失敗時は該当方式の束を作れない旨を明示 (既存 `glslc unavailable` と同じ書き方) |
| Dawn (ネイティブ WebGPU) | 利用者が用意した prebuilt のみ |
| emdawnwebgpu | Emscripten の port 機構 (emsdk 同梱) |

- **事前生成した束の利用も選べる**: ツールが無い環境 (CI の GCC ジョブ等) 向けに、束を生成済みのディレクトリを
  `PICTOR_SHADER_BUNDLE_DIR` で渡せる。リポジトリには束をコミットしない (生成物の二重管理を避ける)。

#### CI

- 既存 `build.yml` (ubuntu / GCC) は維持し、**GCC で RHI 層 (Vulkan 方式 + null 方式) がビルドできること**を必須にする。
  MSVC で通って GCC で落ちた前例への対策として、方式中立の層 (`src/rhi/common/`、`pictor/gfx/`) は GCC で必ずコンパイルされる配置にする。
- 追加: Emscripten のコンパイルのみジョブ (WebGPU + WebGL2、`pictor_core`)。実行はしない (`portability/web.md` 決定 4 と同じ)。
- 追加: 公開ヘッダ検査ジョブ (§4 S0)。
- D3D12 / Metal は GitHub の Windows / macOS ランナーでコンパイルのみ (費用は §5 U-5)。

---

## 4. 段階計画と受入条件 (設問 11)

各段階は単独で main に入れられ、既存の Vulkan 経路を退行させない。テスト・起動・実機確認は人間の指示があるときだけ行い、
未確認のものは未確認と書く。

| 段階 | 内容 | 受入条件 (テスト) | 実機で確かめること |
|---|---|---|---|
| **S0 公開面と検査** | `pictor/gfx/` の型 (ハンドル・記述子・`BackendReport`・`DeviceCaps`・`PassRecorder`) を定義。null 方式 (`PICTOR_RHI_NULL`: 記録だけして何も描かない、headless テスト用)。公開ヘッダ検査を CI に入れる | (1) **公開ヘッダ検査**: `pictor/gfx/` と新しい公開ヘッダ群に `Vk*` / `VK_*` / `ID3D12*` / `IDXGI*` / `MTL*` / `CAMetal*` / `WGPU*` / `wgpu*` / `GL*` / `<vulkan/` / `<d3d12` / `<Metal/` / `<webgpu/` / `<GLES3/` が 0 件 (トークン単位の grep。許可リストは `pictor/backend/*_interop.h` のみ)。(2) **方式マクロ非依存検査**: 公開ヘッダを `PICTOR_HAS_*` 未定義で単独コンパイルできる。(3) PassRecorder の記録内容の単体テスト | なし |
| **S1 Vulkan を口の内側へ** | `src/rhi/vulkan/` へ VulkanContext・CompiledGraph の Vk 部分・GpuTimer・scheduler 記録を移す。CompiledPass を方式中立に。DrawStream。旧 API は interop ヘッダで互換 | 既存 headless テストが全部通る。CompiledPass / DrawStream の構築テスト (null 方式)。`sizeof(CompiledPass)` の static_assert。GCC / MSVC のビルド | 既存 demo (Windows / Linux) の見た目が変わらないこと (人間確認。未確認なら未確認と記録) |
| **S2 シェーダ束** | `pictor_shaderc`、規約 lint、push constant → group 3 の移行、`pictor_add_shader_bundle`、Visus の解決先変更 | 全シェーダが lint を通る。SPIR-V 出力が S1 と同じ反射結果。束の読み込みテスト。Visus の metadata と反射の不一致が明示エラーになるテスト | Vulkan demo の見た目不変 (人間確認) |
| **S3 WebGPU** | `src/rhi/webgpu/` (emdawnwebgpu / 任意 Dawn)。非同期初期化。Tier B の能力照会と降格規則 (§3.2) の実装。Web canvas provider | 降格解決の単体テスト (能力表を差し替えて、Replaced / Omitted / 失敗 が期待どおり)。Emscripten コンパイル。`BackendReport` が FrameTap に載るテスト | Chrome / Edge の WebGPU で基本 preset が描ける (人間確認)。Firefox / Safari は未確認と明記 |
| **S4 WebGL2** | `pictor_webgl` の下回りを `src/rhi/webgl2/` へ。Tier A。`WebGLRenderer` / `WebMeshRenderer` を `RenderSystem` 経由へ。web-mesh の JS 契約不変 | Tier A の能力表で全 preset の降格解決が「成功または宣言どおりの失敗」になるテスト。web-mesh の既存テストが通る | WebGPU を無効にしたブラウザで同じ消費側コードが WebGL2 で描け、降格表示が出る (人間確認) |
| **S5 本体の wasm 構成** | `pictor_core` 切り出し、`pictor.h` から Vulkan include 除去、SingleThreadDispatcher、MEMFS 資産 | Emscripten で本体 + WebGPU + WebGL2 がコンパイルできる。デスクトップの既存テストが通る | Ludellus-Native 段階 5 側で確認 (Pictor 側では最小サンプル host のみ。人間確認) |
| **S6 DX12** | `Dx12Context` を `src/rhi/d3d12/` へ移設、root signature、DXIL 束、SHaRC の束化 | Windows MSVC ビルド。null 方式との記録一致テスト (同じ DrawStream から同じ数の draw を発行) | Windows 実機で基本 preset が描ける / device removed からの復旧 (人間確認) |
| **S7 Metal** | `MetalContext` を `src/rhi/metal/` へ移設、MSL 束、macOS 対応 | macOS ランナーでコンパイル (CI 導入は U-5) | iOS / macOS 実機 (人間確認。今まで iOS はビルド・実機とも未確認) |
| **S8 旧 API 撤去** | interop ヘッダの旧経路、`NativeDrawCallback`、`get_required_instance_extensions`、旧 c_api を削除 | 公開ヘッダ検査の許可リストから旧経路が消える。Ergo / Ludellus-Native が新公開面でビルドできる | 主要消費側の動作確認 (人間確認) |

- S3 と S4 の順は入れ替え可能 (WebGL2 は既存資産があるので早く見える)。ただし降格規則は Tier B / A の両方で
  使うので、先に入れる方で実装する。
- 「消費側が方式を知らない」の機械的な検査は S0 の (1)(2) に加えて、**同一サンプル host の検査**を S4 以降に足す:
  汎用サンプル host (ゲーム非依存) 1 本を、ソース無変更で `-DPICTOR_RHI_*` の組み合わせを変えてビルドする CI 行列。
  ソースに方式名の分岐が入ったら検査で落とす (`grep` で `PICTOR_HAS_` / 方式名文字列が 0 件)。

---

## 5. リスク

| ID | リスク | 影響 | 推奨の対策 |
|---|---|---|---|
| R-1 | S1 (Vulkan の移設) がレンダ経路全面に及び、退行を見落とす | 既存 consumer の描画が壊れる | 移設中は旧経路と新経路を同じ binary に置かず、段階ごとに切り替える。FrameTap の描画リスト出力を移設前後で突き合わせる (headless で比較できる) |
| R-2 | SPIR-V → HLSL → DXIL 経路の変換失敗・性能差 | D3D12 で一部シェーダが動かない | 方式別の上書きソース (今の `sharc/hlsl/` 方式) を束の仕組みで許す。上書きを使ったことは束のメタデータに残す。恒常化したら Slang への切替を検討 (U-2) |
| R-3 | WebGPU の能力差 (storage 上限、`maxBindGroups = 4`、timestamp 無し) | 既存 preset が Tier B で通らない | 4 グループ固定のモデル (§3.3) を Vulkan でも同じに揃え、方式差を作らない。preset ごとに Tier B / A の解決結果を CI の単体テストで固定する |
| R-4 | WebGPU の非同期初期化が既存の同期 `initialize()` 前提の host と合わない | host 側の修正が要る | デスクトップでは `initialize()` が同期で完了する互換を残し、`Pending` は Web だけで返る。ただし消費側コードを同じにするため、host は常に `poll_initialize()` を呼ぶ書き方を推奨形として示す |
| R-5 | wasm の大きさ (WebGPU + WebGL2 の 2 方式同梱) | 配信サイズ | 2 方式とも薄い (シェーダコンパイラを入れない)。大きさの予算は Ludellus-Native 段階 5 で決め、超えたら WebGL2 を別 wasm に分ける選択肢を残す |
| R-6 | DeviceLost 復旧で資源の作り直し素材が無い | 復旧できない資源が出る | `ResourceLost` 通知と `retain_cpu_copy` (U-4)。復旧できないものは黙って空にせず通知 |
| R-7 | 公開面の設計が Ergo の現行構造 (FrameComposer / IRenderLayer) と合わず、Ergo 側の移行が重い | 互換期間が長引く | 互換期間を設け (§3.9)、Ergo 側の移行設計は Ergo のタスクとして別に起こす。Pictor から Ergo へは依存しない |
| R-8 | macOS / iOS の CI が無い | Metal 方式が腐る | S7 で macOS ランナーのコンパイルのみジョブ (U-5) |
| R-9 | Dawn の prebuilt を用意する手間 | ネイティブで WebGPU 方式を試せない | ネイティブ Dawn は任意。Web での確認を正とする |
| R-10 | 公開ヘッダ検査の grep が誤検出 (例: コメント中の `VkBuffer`) | 検査の形骸化 | トークン検査 + コメント除去。許可リストはファイル単位で狭く持つ |

## 6. 未決事項 (推奨つき) (設問 12)

### 人間 (neco) にしか決められないもの

| ID | 論点 | 推奨 |
|---|---|---|
| U-1 | Windows の既定方式 (Vulkan / D3D12) と macOS の既定 (MoltenVK / Metal) | Windows は当面 Vulkan、macOS は当面 MoltenVK。D3D12 / Metal が全 preset 対応に達した時点で再判断 |
| U-2 | シェーダの正本言語を将来 Slang へ移すか | 今は GLSL 継続 (移行量 0)。R-2 の上書きが常態化したら Slang を判断 |
| U-3 | デスクトップ OpenGL / Android GLES 予備を作るか | 作らない。必要が出たら WebGL2 方式を GLES3 として流用する案を先に検討 |
| U-4 | DeviceLost 時に CPU 側の写しを Pictor が持つか (メモリと引き換え) | 既定は持たない (Pictor のローダ経由の資源は出所から読み直し、消費側が渡した資源は `ResourceLost` 通知)。Web / モバイルの host が必要なら `retain_cpu_copy` を ON |
| U-5 | macOS / Windows の CI ランナー費用を出すか | Metal / D3D12 段階 (S6 / S7) の開始時にコンパイルのみで導入 |
| U-6 | `feat/native-metal-dx12-recovery` ブランチを閉じるか | 閉じる (内容は main に入っている。§1.3)。削除は不可逆寄りなので人間判断 |
| U-7 | 互換期間 (旧 Vulkan API を残す期間) の終わりの条件 | Ergo と Ludellus-Native が新公開面へ移り終えた時点。日付では決めない |

### 実装段階で決めてよいもの (推奨で進める)

| ID | 論点 | 推奨 |
|---|---|---|
| D-1 | Vulkan の dynamic rendering を既定にするか | 使える端末では使うが、`VkRenderPass` 経路を既定に残す (Android の古い端末) |
| D-2 | Metal の binding を argument buffer にするか | 初期は直接添字。性能計測後に argument buffer |
| D-3 | WebGPU の indirect draw count の代替 | compute で上限まで 0 埋めした固定数の indirect (Emulated として記録) |
| D-4 | 束のファイル形式 | 方式ごとの blob + 反射 JSON をまとめた 1 ファイル。版番号を持ち、読み込み時に版不一致を明示エラー |
| D-5 | null 方式を製品ビルドに入れるか | テスト専用。製品ビルドでは OFF |

---

## 7. 参照

- `spec/architecture/framework-boundaries.md` (PC-RULE-001〜003)
- `spec/feature/dx12-backend-design.md` / `spec/feature/metal-backend-design.md` (本案で RHI 部分を置き換える前提の既存設計)
- `spec/feature/platform-feature-matrix.md` / `spec/feature/portability.md` / `spec/feature/portability/web.md` / `spec/feature/portability/mobile.md`
- `spec/feature/subsystem/surface.md` / `spec/feature/portability/mobile-surface-recovery.md`
- `include/pictor/pipeline/compiled_graph.h` / `include/pictor/pipeline/render_pass_scheduler.h` / `include/pictor/profiler/gpu_timer.h`
- `include/pictor/surface/{surface_provider,frame_result,context_init_result,dx12_context,metal_context,native_render_config}.h`
- `include/pictor/webgl/webgl_renderer.h` / `include/pictor/visus/visus_shader_package.h` / `include/pictor/c_api.h`
- 消費側: `Ludellus-Native/spec/feature/native-first-web.md`、Ergo `include/ergo/render/` (FrameComposer / IRenderLayer / render_backend)、Ergo `include/ergo/gpu_particle/gpu_backend.h`
