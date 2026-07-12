# Pipeline System-B Configuration (scene-side, Phase 3)

設計書 — 2026-05-23 起草。系統B (実 `VkRenderPass` / framebuffer / attachment / draw 記録) の scene 側ハードコードを解体し、`*.profile.json` から駆動できるようにする。`spec/rendering-extensibility-design.md` §6.4 後継のフェーズ 3。

post-process 側の系統B は Phase 2 §6.3 (`feat/postprocess-chain`) で既に汎用チェーン化済み。本書の対象は **scene render pass + 中間 attachment + scheduler execute()**。

## 1. スコープと非スコープ

### 1.1 対象 (Phase 3 で配線するもの)

| 項目 | 現状 (Phase 2 終了時点) | Phase 3 のゴール |
|------|------------------------|------------------|
| scene render pass | `vulkan_context.cpp` の `create_render_pass()` で固定の HDR color + depth attachment + load=CLEAR / store=STORE をハードコード | `RenderPassConfig` (JSON) から attachment 列・load/store op・サブパス・依存を構築 |
| swapchain render pass | 同上、固定の swapchain color + load=LOAD で hardcoded | 同上、別 `RenderPassConfig` で表現 |
| 中間 attachment (`scene_hdr_color` / `scene_depth` / `swapchain`) | `swapchain.cpp` で作成、名前は文字列定数 (scanner からは見えるが C++ は文字列を共有していない) | `AttachmentRegistry` (JSON で宣言 + C++ で作成) で名前→`VkImage`/`VkImageView`/format/extent を引ける |
| framebuffer | `swapchain.cpp` が swapchain 用、`postprocess_pipeline.cpp` が中間用と 2 系統に分散 | `FramebufferRegistry` 一本化、render pass 名 + attachment 名で resolve |
| `RenderPassScheduler::execute()` | 各 `PassType` の case がコメントスタブ | `pass_name` で `RenderPassConfig` を引き、設定された `render_targets` の framebuffer に `vkCmdBeginRenderPass`、`input_textures` を descriptor に bind |
| `shader_override` | `RenderPassDef` に格納されるが scheduler 未消費 | `ShaderRegistry::pipeline(handle)` を resolve して `vkCmdBindPipeline` |

### 1.2 非スコープ (Phase 3 で触らない)

- post-process 側系統B (Phase 2 §6.3 で完了)
- draw 記録の中身そのもの (どのメッシュをどの順に描くか — `RenderBatch` 構築は KS の責務、Pictor 側は順序保証だけ)
- SPIR-V reflection / シェーダ自動 introspection (`layout(location=N)` 不一致は validation layer 任せ、Phase 2 §6.2 同様)
- dynamic rendering (`VK_KHR_dynamic_rendering`) への移行 — 現実装は VkRenderPass + Framebuffer ベース。将来別タスク
- TAA history (Phase 2 §6.3 の積み残しと同じ — フレーム間生存 attachment の汎用化)
- compute pipeline (`PassType::COMPUTE` の中身配線) — Phase 4

## 2. アーキテクチャ全体図

```
*.profile.json
  └─ attachments[]      ← 名前付き attachment 宣言 (format/usage/clear)
  └─ render_passes[]    ← 既存 RenderPassDef を拡張 (load/store op を追加)
  └─ framebuffers[]     ← (任意) 明示宣言。省略時は render pass + attachment 名から自動

C++ 起動順:
  1. PipelineProfileBuilder が attachments[] を AttachmentRegistry へ
  2. VulkanContext::create_swapchain() の中で AttachmentRegistry が swapchain 画像を attach
  3. VulkanContext::create_render_passes() が render_passes[] を回り
     RenderPassConfig 毎に VkRenderPass を生成し RenderPassRegistry へ登録
  4. FramebufferRegistry が render_passes[].render_targets + AttachmentRegistry から
     framebuffer を作成 (swapchain は per-image)

Init 時 (プロファイル切替 1 回):
  PipelineCompiler が profile.render_passes[] (string 参照だらけ) を
  compile() し、 std::vector<CompiledPass>(SoA-ish AoS) を作る。 全
  string → index / handle を解決:
    - attachment 名 → AttachmentRegistry の uint16_t index
    - shader_override → VkPipeline (直値)
    - render_pass / framebuffer → VkRenderPass / VkFramebuffer (直値)
    - input_textures → 事前構築済 VkDescriptorSet (per-flight)
    - filter_mask / sort_mode → 既に数値、 そのまま

Per-frame (RenderPassScheduler::execute) — hot path に string / map lookup ゼロ:
  for (const CompiledPass& cp : compiled_.passes) {
      vkCmdBindDescriptorSets(... cp.input_sets[flight] ...);   // 事前 build
      vkCmdBeginRenderPass(cmd, &cp.rp_begin_info[image_index], ...);
      if (cp.pipeline != VK_NULL_HANDLE)
          vkCmdBindPipeline(cmd, ..., cp.pipeline);
      record_batches(cmd, cp.filter_mask, cp.sort_mode);
      vkCmdEndRenderPass(cmd);
  }
```

**ノード化のオーバーヘッド方針** (`feedback_pictor_dod_layout` 準拠):
- JSON の `render_passes[]` は宣言形式 (string 名で参照、可読性優先)
- 起動時 `PipelineCompiler::compile()` で **CompiledGraph** (`std::vector<CompiledPass>`、 各エントリは VkHandle / index 直値のみ) へ畳み込む
- `RenderPassScheduler::execute()` は **CompiledGraph をフラットイテレートするだけ**。 `unordered_map<string, ...>` / `std::string` / `find_by_name` は hot path に出ない
- プロファイル切替・swapchain resize 時のみ `compile()` をやり直す (init 1 回方針)
- `CompiledPass` は AoS 構造体だが、 主要 VkHandle 3 個 + bitmask 数個で 64-128 B 程度に収まる。 SIMD ではなく seq-iteration なので AoS のままキャッシュ効率が高い

## 3. データモデル (C++ 側)

### 3.1 `AttachmentDef` (新規、`include/pictor/pipeline/attachment_def.h`)

```cpp
enum class AttachmentKind : uint8_t {
    COLOR,             // 通常 color
    DEPTH,             // depth or depth-stencil
    SWAPCHAIN_COLOR,   // 特別: ランタイムが swapchain image を inject
};

enum class AttachmentSizing : uint8_t {
    SWAPCHAIN_RELATIVE,  // extent = swapchain_extent * scale
    ABSOLUTE,            // extent = (width, height) 固定
};

struct AttachmentDef {
    std::string      name;            // unique 名 ("scene_hdr_color" / "scene_depth" / "swapchain")
    AttachmentKind   kind;
    VkFormat         format;          // VK_FORMAT_R16G16B16A16_SFLOAT 等
    AttachmentSizing sizing;
    float            scale;           // SWAPCHAIN_RELATIVE 用 (既定 1.0)
    uint32_t         width, height;   // ABSOLUTE 用
    VkImageUsageFlags usage;          // COLOR_ATTACHMENT_BIT | SAMPLED_BIT 等
    VkClearValue     clear_value;     // load_op=CLEAR 時の値
};
```

JSON 表現:
```json
"attachments": [
  {
    "name": "scene_hdr_color",
    "kind": "COLOR",
    "format": "R16G16B16A16_SFLOAT",
    "sizing": "SWAPCHAIN_RELATIVE",
    "scale": 1.0,
    "usage": ["COLOR_ATTACHMENT", "SAMPLED"],
    "clear_color": [0.0, 0.0, 0.0, 1.0]
  },
  {
    "name": "scene_depth",
    "kind": "DEPTH",
    "format": "D32_SFLOAT",
    "sizing": "SWAPCHAIN_RELATIVE",
    "scale": 1.0,
    "usage": ["DEPTH_STENCIL_ATTACHMENT"],
    "clear_depth": 1.0
  },
  {
    "name": "swapchain",
    "kind": "SWAPCHAIN_COLOR",
    "format": "B8G8R8A8_SRGB",
    "sizing": "SWAPCHAIN_RELATIVE",
    "scale": 1.0,
    "usage": ["COLOR_ATTACHMENT"]
  }
]
```

予約名は `swapchain` のみ (`SWAPCHAIN_COLOR` kind と対応、ランタイムが per-frame image を差し込む)。

### 3.2 `RenderPassDef` 拡張 (`include/pictor/pipeline/pipeline_profile.h`)

既存フィールド (`pass_name` / `pass_type` / `shader_override` / `render_targets` / `input_textures` / ...) を維持しつつ、以下を追加:

```cpp
struct AttachmentOps {
    std::string       attachment_name;
    VkAttachmentLoadOp  load_op   = VK_ATTACHMENT_LOAD_OP_CLEAR;
    VkAttachmentStoreOp store_op  = VK_ATTACHMENT_STORE_OP_STORE;
    VkImageLayout     initial_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImageLayout     final_layout   = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
};

struct RenderPassDef {
    /* 既存フィールド ... */
    std::vector<AttachmentOps> attachment_ops;  // render_targets と同サイズが期待
    // attachment_ops が空なら render_targets だけから既定 (CLEAR/STORE) を推論
};
```

JSON 拡張:
```json
{
  "pass_name": "Scene HDR",
  "pass_type": "OPAQUE",
  "render_targets": ["scene_hdr_color", "scene_depth"],
  "attachment_ops": [
    {"attachment": "scene_hdr_color", "load": "CLEAR", "store": "STORE",
     "initial_layout": "UNDEFINED", "final_layout": "SHADER_READ_ONLY_OPTIMAL"},
    {"attachment": "scene_depth", "load": "CLEAR", "store": "DONT_CARE",
     "initial_layout": "UNDEFINED", "final_layout": "DEPTH_STENCIL_ATTACHMENT_OPTIMAL"}
  ],
  "input_textures": [],
  "shader_override": "none"
}
```

`attachment_ops` 省略時は既定値で `render_targets` から推論 (color は CLEAR/STORE、depth は CLEAR/DONT_CARE、最終レイアウトは color=SAMPLED、depth=DSV)。後方互換のため。

### 3.3 `AttachmentRegistry` (新規、`include/pictor/pipeline/attachment_registry.h`)

```cpp
class AttachmentRegistry {
public:
    void initialize(VkDevice device, const std::vector<AttachmentDef>& defs,
                    VkExtent2D swapchain_extent, uint32_t flight_count);
    void resize(VkExtent2D new_extent);
    void shutdown();

    // init 時の名前解決にだけ使う。 hot path では呼ばない。
    // 戻り値 0xFFFF = 未登録 (PipelineCompiler が fatal log)
    uint16_t    index_of(std::string_view name) const;
    VkImageView view(uint16_t index, uint32_t frame_index) const;
    VkImage     image(uint16_t index, uint32_t frame_index) const;
    VkFormat    format(uint16_t index) const;
    VkExtent2D  extent(uint16_t index) const;
    const AttachmentDef* find(uint16_t index) const;

    // ランタイムが swapchain image を inject
    void set_swapchain_images(std::span<const VkImage> images,
                              std::span<const VkImageView> views,
                              VkFormat format, VkExtent2D extent);
};
```

per-flight (flight_count = 2~3) で多重化、`resize()` で extent 追従。SWAPCHAIN_COLOR だけは swapchain 側からの inject に従う (`VulkanContext::recreate_swapchain` から呼ぶ)。
**index 引きが正規** — `std::string` を hot path に出さないため、 lookup は init 時に 1 回だけ `index_of()` で `uint16_t` に解決して、 以降はその index を使い回す。

### 3.4 `RenderPassRegistry` / `FramebufferRegistry`

```cpp
class RenderPassRegistry {
public:
    void initialize(VkDevice device, const std::vector<RenderPassDef>& passes,
                    const AttachmentRegistry& attachments);
    uint16_t     index_of(std::string_view pass_name) const;
    VkRenderPass get(uint16_t index) const;
    void shutdown();
};

class FramebufferRegistry {
public:
    void initialize(VkDevice device, const std::vector<RenderPassDef>& passes,
                    const RenderPassRegistry& rps,
                    const AttachmentRegistry& attachments,
                    uint32_t flight_count, uint32_t swapchain_image_count);
    void resize(VkExtent2D new_extent);
    // swapchain attachment を含む pass は per-swapchain-image、それ以外は per-flight
    // PipelineCompiler が compile 時に per-pass で flat 配列を引き写すので
    // hot path はこの get() も呼ばない設計
    VkFramebuffer get(uint16_t pass_index, uint32_t frame_or_image_index) const;
    void shutdown();
};
```

両者は `AttachmentRegistry::resize()` 後に再構築。 hot path から消すために `get()` は init 時にしか呼ばれない。

### 3.5 `CompiledPass` / `CompiledGraph` — hot path 用 flat 構造

`PipelineCompiler::compile()` が `profile.render_passes[]` を全 string 解決後の flat 配列へ畳み込む。 1 フレームの execute は **この配列を seq-iterate するだけ**。

```cpp
struct CompiledPass {
    // ---- 描画コマンド側 (hot path で直値が必要) ----
    VkRenderPass    render_pass;         // RenderPassRegistry から
    // per-image / per-flight の framebuffer 配列。 swapchain attachment を
    // 含む pass は per-swapchain-image (3 枚)、 含まない pass は per-flight
    // (2~3 枚)。 indexing は image_index か flight_index のどちらか
    // — is_swapchain で分岐 (枝予測は 1 種類しか出ない)
    std::array<VkFramebuffer, 4> framebuffers;  // 0..framebuffer_count
    std::array<VkDescriptorSet, 4> input_sets;  // 0..flight_count

    // ---- clear 値 / extent も pre-resolve (BeginRenderPassInfo に渡す) ----
    VkRect2D       render_area;
    uint16_t       clear_count;
    std::array<VkClearValue, 8> clear_values; // max 8 attachments per pass

    // ---- pipeline override (NULL = scheduler が pass_type 既定を使う) ----
    VkPipeline     pipeline;

    // ---- バッチ抽出ヒント (uint で済む。 hot path で OK) ----
    uint32_t       filter_mask;          // RenderBatch filter
    uint8_t        sort_mode;            // FRONT_TO_BACK / BACK_TO_FRONT / NONE
    uint8_t        pass_type;            // PassType enum を 1 byte で
    uint16_t       pass_id;              // pass name を compile 時に解決した callback key
    uint8_t        framebuffer_count;    // 1=per-flight, 3=per-swapchain-image
    uint8_t        is_swapchain : 1;
    uint8_t        is_compute   : 1;
    uint8_t        reserved     : 6;

    // ---- 観測用 (Phase 2 §6.1 の GPU timestamp タグ) ----
    uint16_t       timestamp_begin_query;
    uint16_t       timestamp_end_query;
    const char*    debug_name;           // string_view 化した名前 (static-lived)
};
static_assert(sizeof(CompiledPass) <= 192,
              "CompiledPass should stay cache-line friendly");

struct CompiledGraph {
    std::vector<CompiledPass> passes;            // 実行順
    // descriptor pool / input_set 配列の所有も graph がまとめて持つ
    VkDescriptorPool descriptor_pool;
    std::vector<VkDescriptorSet> descriptor_sets_storage;
    // push constant データのバックストア (CompiledPass からは offset+size で参照)
    std::vector<std::byte> push_constants_storage;
};

class PipelineCompiler {
public:
    static CompiledGraph compile(const PipelineProfileDef& profile,
                                 const AttachmentRegistry&,
                                 const RenderPassRegistry&,
                                 const FramebufferRegistry&,
                                 const ShaderRegistry&,
                                 VkDevice device,
                                 uint32_t flight_count,
                                 uint32_t swapchain_image_count);
};
```

**hot path 不変条件:**
- `RenderPassScheduler::execute()` は `for (const auto& cp : graph.passes)` だけ。 `.find()` / `unordered_map` / `std::string` は 0 個
- `CompiledPass` 内に解決済 VkHandle / index / push constant offset のみ。 attachment 名 / pass 名 / shader 名は文字列としては保持しない (debug_name は static literal の `const char*` のみ、 hot path で読まない)
- `descriptor_sets_storage` は init 時に一括 allocate (pool フラグメント抑止)、 `input_sets` は span でなく 4 要素固定配列 (flight_count ≤ 4 を上限とする)
- profile 切替 / swapchain resize 時のみ `compile()` を再実行 (init 1 回方針)

これで `RenderPassScheduler::execute()` の per-frame 計算量は「pass 数 × 各 vkCmd* 呼出」 のみ、 余分な間接参照ゼロ ([[feedback_pictor_dod_layout]])。

## 4. C++ 側のリファクタリング順

### 4.1 ステップ A: AttachmentRegistry を導入 (互換維持)

- `AttachmentDef` / `AttachmentRegistry` を新設
- `VulkanContext` に `AttachmentRegistry attachments_` を追加
- 既存 `scene_render_pass_` は AttachmentRegistry が **空でも** 作成できるよう、profile に attachments がない場合は既定 (`scene_hdr_color` + `scene_depth` + `swapchain`) を C++ 側でハードコード fallback
- `PipelineProfileBuilder` が `AttachmentDef[]` ビルダーを提供

検証: 既存 KS が無改変で起動できる。`*.profile.json` に attachments を書かなくても従来通り動く。

### 4.2 ステップ B: RenderPassRegistry / FramebufferRegistry を導入

- 既存 `vulkan_context.cpp` の `create_render_pass()` を `RenderPassRegistry::initialize()` に移植
- 既存 `create_framebuffers()` を `FramebufferRegistry::initialize()` に移植
- profile に `render_passes[]` がない場合のフォールバックとして「Scene HDR」「Swapchain Composite」 2 件を C++ 側で自動生成

検証: 既存 KS が無改変で起動できる。プロファイルに `render_passes[]` を書くと、その通りの順序で VkRenderPass が生成される。

### 4.3 ステップ C: PipelineCompiler + RenderPassScheduler::execute() を実描画化

- `PipelineCompiler::compile()` を新設 (§3.5)。 起動時 / プロファイル切替 / swapchain resize で 1 回だけ呼ぶ。 `profile.render_passes[]` を全 string 解決して `CompiledGraph` (`std::vector<CompiledPass>`) へ畳み込む。
- `RenderPassScheduler` は `CompiledGraph` を保持し、 `execute(cmd)` は **フラットイテレートのみ**:
  - `vkCmdBindDescriptorSets(... cp.input_sets[flight] ...)` (pre-built set)
  - `vkCmdBeginRenderPass(cmd, &cp.rp_begin[image_index|flight] ...)`
  - `if (cp.pipeline) vkCmdBindPipeline(...)` (compile 時に NULL なら pass_type 既定パイプラインを scheduler が default として持つ)
  - `vkCmdDispatch` or `record_batches(cmd, cp.filter_mask, cp.sort_mode)` を pass_type で分岐 (`switch` 1 個、 hot path で 1 fn ptr ジャンプ程度)
  - `vkCmdEndRenderPass(cmd)`
- pass_type ごとの記録経路:
  - `OPAQUE` / `TRANSPARENT` / `DEPTH_ONLY` / `SHADOW`: `record_batches` が RenderBatch を `vkCmdDraw*` でフラッシュ
  - `POST_PROCESS`: pass_type は CompiledPass にすでに反映済。 既存 `PostProcessPipeline` の chain 経路を CompiledPass.pipeline + framebuffer で駆動 (互換維持)
  - `COMPUTE`: `vkCmdDispatch(cmd, group_x, group_y, group_z)`、 dispatch サイズは CompiledPass に pre-resolve
- pass 固有 callback は `CompiledPass::pass_id` を添字にした flat table から選ぶ。複数の COMPUTE pass も per-frame 文字列比較なしで区別する
  - `CUSTOM`: `ICustomRenderPass*` を CompiledPass に pre-resolve しておき `cp.custom->execute(cmd, frame_ctx)`
- `input_textures[]` は compile 時に **per-flight VkDescriptorSet を一括 build**。 hot path は `cp.input_sets[flight]` を bind するだけ。 layout は 1 種類 (sampled image 0..N) を共有
- `shader_override` は compile 時に `ShaderRegistry::pipeline(handle)` 解決 → `cp.pipeline` に直値

検証: KS が無改変で起動でき、 フレーム出力が従来と pixel-equivalent。 プロファイル切替で compile が走り直して flat graph が差し替わる。 hot path に string lookup が 0 個であること (perfetto trace で `std::string`/`unordered_map` シンボルが per-frame に出ないことを確認)。

#### ステップ C 配線状況 (2026-07-02, `fix/render-compiled-path-wiring`)

| 項目 | 実体 | 状態 |
|---|---|---|
| compile の呼び出し元 | `CompiledPathDriver` (`pipeline/compiled_path_driver.{h,cpp}`) — engage / recompile / disengage で graph ライフサイクル (descriptor pool 解放含む) を管理。 `PictorRenderer::compile_render_graph()` が公開 seam、 プロファイル切替 (`apply_profile`) 時は自動再 compile (毎フレーム compile しない) | ✅ |
| フレームループからの execute_compiled | `PictorRenderer::render_compiled(cmd, flight, image, ...)` — host のコマンドバッファ記録中に呼ぶ。 PassRecordFn 版 (完全 host record) と `CompiledBatchRecorder` 版 (RenderBatch → vkCmdDrawIndexed + 実測統計) の 2 オーバーロード | ✅ |
| record_batches 相当 | `CompiledBatchRecorder` — OPAQUE/TRANSPARENT を `IBatchGpuSource` (host が MeshHandle/shaderKey → VkBuffer/VkPipeline を解決する seam) 経由で記録。 material variant はバッチ単位インライン解決 (per-frame alloc なし)。 SHADOW / DEPTH_ONLY は未実装 — 無言 skip せず 1 回 warn + stats 計上 (§7.1) | ✅ (SHADOW/DEPTH_ONLY は未実装を明示) |
| 旧 managed `execute()` | custom pass 実行のみに縮退。 built-in pass は compiled graph 未設置なら 1 回だけ明示 warn (D-2 のサイレント no-op を解消)。 `remap_batches_for_pass` (M-2 per-frame alloc) は削除 | ✅ |
| draw call / triangle 統計 | recorder の実測値を `render_compiled()` が profiler へ集計。 GPUDrivenPipeline の placeholder 統計 (visible=count / draw_calls=1) は「未計測 = 0」へ是正 | ✅ |
| headless 統合テスト | `tests/unit_compiled_graph_wiring_test.cpp` — headless compile (device=NULL) の graph 構造 / execute_compiled の record 順序 / driver ライフサイクル / recorder 統計 / プロファイル切替再 compile | ✅ |
| KS 側配線 | `kuzu.profile.json` + registries 構築 → `compile_render_graph()` 呼び出し | 未 (KS 側 PR、 §9) |

### 4.4 ステップ D: 既存呼び出し側の整理

- `FrameComposer` を `RenderPassScheduler::execute()` 経由に統合 (HUD レイヤーは pass として scheduler 内に置く / または "post-scheduler hook" として残す)
- KS の `GameRenderer` / `SkinnedLayer` / `PostProcessLayer` / `HudLayer` を pass 単位に整理 (大改修になるので KS 側は別 PR に分ける可能性)

検証: KS 起動 + プロファイル差し替えでフレーム構成が変わる。

## 5. JSON スキーマバージョン

現行 v1 → v2。`version: 2` 以降で attachments / attachment_ops を消費。`version: 1` プロファイル (Phase 2 までに作った 5 プリセット) は loader 側で既定 attachments を補完して動かす。

スキーマ正本: 本書 + `spec/pipeline-profile-config.md` の §3 を v2 で更新。

## 6. リスクとフォールバック

| リスク | 検知 | フォールバック |
|--------|------|----------------|
| attachment 名typo で resolve 失敗 | `RenderPassRegistry::initialize()` で fatal log + 既定 attachments で起動継続 | "scene_hdr_color" / "scene_depth" / "swapchain" の 3 名を built-in 既定として保持 |
| `attachment_ops` 不整合 (subpass dep 違反) | validation layer エラー | デフォルト推論 (CLEAR/STORE + SHADER_READ_ONLY 最終) にフォールバック |
| ステップ C の draw 記録パスが従来と差分 | KS で目視 + screenshot 比較 (Custos 連携) | ステップ B で stop、`feat/pipeline-system-b` を merge せず差分原因特定 |
| 既存プロファイル (v1, attachments 未宣言) の互換切れ | loader が version 検出して既定補完 | v1 互換シム (4.1, 4.2 のフォールバック) |
| resize で attachment 取りこぼし | swapchain recreate 時に AttachmentRegistry::resize → 全 framebuffer rebuild | C++ assertion で resize 失敗時は古い framebuffer を破棄しない (フレーム skip) |

## 7. テスト計画

- **CTest 単体**: `tests/unit_attachment_registry_test.cpp` (alloc / resize / swapchain inject)、`tests/unit_render_pass_registry_test.cpp` (config から VkRenderPass 生成、load/store op 反映)、`tests/unit_framebuffer_registry_test.cpp`、`tests/unit_pipeline_profile_serializer_test.cpp` 拡張 (v2 round-trip + v1 後方互換)
- **統合**: 既存 `tests/unit_pipeline_profile_round_trip_test.cpp` に attachments[] + attachment_ops[] を含むケース追加
- **ホスト**: KS が無改変で起動 + Custos でフレーム比較。プロファイル切替 ("standard" → "high" 等) で attachments 差分が反映されることを確認

## 8. Ergo plugin 側の編集 UI

Phase 3 では系統B (実 VkRenderPass チェーン) を解体し JSON 駆動にするため、 **Pictor のソースを静的 scan する意味が無くなる**。 Ergo の Scanner モード (Python regex + `scanner/render_pipeline.json`) は **削除**。 Profile が single source of truth。

Profile Editor / Timeline / DAG は **単一のグラフエディタ画面に統合**する:
- ノード = `render_passes[]` の各 pass
- ノード端子 = pass の `render_targets` (出力) / `input_textures` (入力)
- エッジ = attachment 名で接続 (drag-to-connect で attachment 経由の依存を貼る)
- timing (Phase 2 §6.1) は同グラフへ overlay 色 / バー (`{op:"timing"}` を受けたら各ノードを着色)
- アニメーションは無効 (`physics: false` / `smooth: false`、 ノードは静的レイアウト)

詳細は Ergo 側 `spec/tool/render_pipeline_system_b.md` を参照。

## 9. KS 側の対応

KS は本書の対象外だが、ステップ D で `data/render/kuzu.profile.json` に attachments[] と attachment_ops[] を追加する PR を別建てする。Pictor 側のフォールバック (4.1 / 4.2) があるので KS 側 PR は Pictor merge の後追いで OK。

`spec/rendering_overview.md` に「Phase 3: scene-side 系統B」を追記 (KS 側 PR の中で)。

## 10. 実装スコープ見積もり

- Pictor 側: 新規ヘッダ 5 (`attachment_def.h` / `attachment_registry.h` / `render_pass_registry.h` / `framebuffer_registry.h` / `pipeline_compiler.h`) + 実装 5 + 既存 `vulkan_context.cpp` / `render_pass_scheduler.cpp` / `pipeline_profile.h` / `pipeline_profile_serializer.cpp` の改修。新規 CTest 5 ファイル (compiler の不変条件テスト含む — hot path に string 系シンボルが出ないことを check)。
- Ergo 側: scanner 削除 (`scanner/render_pipeline_scan.py` / `scanner/render_pipeline.json`)、 plugin を「単一グラフエディタ」 に再構築 (`profile_schema.ts` を v2 化 + DAG ↔ ノード ↔ Profile の単一データ源)、 アニメ無効化。
- KS 側: `data/render/kuzu.profile.json` の attachments[] 明示化 + `spec/rendering_overview.md` 追記 (別 PR)。

## 11. ブランチ + PR

- Pictor: `feat/pipeline-system-b` (本書 + Phase 3 実装一式、1 PR)
- Ergo: `feat/render-pipeline-system-b` (Profile Editor v2 + scanner v2 schema 認識、1 PR)
- KS: 後追いで別 PR
