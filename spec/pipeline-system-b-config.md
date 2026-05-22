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

Per-frame (RenderPassScheduler::execute):
  for pass in profile.render_passes:
    rp = RenderPassRegistry.get(pass.pass_name)
    fb = FramebufferRegistry.get(pass.pass_name, frame_index)
    bind input_textures → descriptor
    vkCmdBeginRenderPass(rp, fb, ...)
    if pass.shader_override != "none":
        vkCmdBindPipeline(ShaderRegistry.pipeline(pass.shader_override))
    record_batches(pass.filter_mask)
    vkCmdEndRenderPass
```

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

    // SWAPCHAIN_COLOR は per-image だが、それ以外は per-flight (or 単一)
    VkImageView view(const std::string& name, uint32_t frame_index) const;
    VkImage     image(const std::string& name, uint32_t frame_index) const;
    VkFormat    format(const std::string& name) const;
    VkExtent2D  extent(const std::string& name) const;
    const AttachmentDef* find(const std::string& name) const;

    // ランタイムが swapchain image を inject
    void set_swapchain_images(std::span<const VkImage> images,
                              std::span<const VkImageView> views,
                              VkFormat format, VkExtent2D extent);
};
```

per-flight (flight_count = 2~3) で多重化、`resize()` で extent 追従。SWAPCHAIN_COLOR だけは swapchain 側からの inject に従う (`VulkanContext::recreate_swapchain` から呼ぶ)。

### 3.4 `RenderPassRegistry` / `FramebufferRegistry`

```cpp
class RenderPassRegistry {
public:
    void initialize(VkDevice device, const std::vector<RenderPassDef>& passes,
                    const AttachmentRegistry& attachments);
    VkRenderPass get(const std::string& pass_name) const;
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
    VkFramebuffer get(const std::string& pass_name, uint32_t frame_or_image_index) const;
    void shutdown();
};
```

両者は `AttachmentRegistry::resize()` 後に再構築。

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

### 4.3 ステップ C: RenderPassScheduler::execute() を実描画化

- 各 case の中身を実装:
  - `OPAQUE` / `TRANSPARENT` / `DEPTH_ONLY` / `SHADOW`: framebuffer + render pass 引いて `vkCmdBeginRenderPass` → batch を `vkCmdDraw*` (RenderBatch ベース)
  - `POST_PROCESS`: 既存 `PostProcessPipeline::record` を chain ベースで呼ぶ (互換維持)
  - `COMPUTE`: Phase 3 では `vkCmdDispatch` のみ、descriptor 系は Phase 4
  - `CUSTOM`: 既存 `ICustomRenderPass::execute()` 呼出を維持
- `input_textures[]` は descriptor set 0 の binding 0..N に sampled image として bind (固定 layout、Phase 4 で柔軟化)
- `shader_override` が "none" 以外なら `ShaderRegistry::pipeline(handle)` を bind

検証: KS が無改変で起動でき、フレーム出力が従来と pixel-equivalent。`*.profile.json` で pass 順を入れ替えると実描画も追従する。

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

## 8. Ergo plugin 側の編集 UI 拡張

`tools/ergo/src/plugins/render_pipeline/` の Profile Editor を v2 スキーマに対応させる。詳細は Ergo 側 `spec/tool/render_pipeline.md` §「Profile Editor v2 (Phase 3)」を参照。

主な追加 UI:
- Attachments タブ (現状の Scanner ビューの Attachments タブとは別に、Profile Editor 側にも attachments 編集を置く)
- 各 RenderPass エントリに attachment_ops のグリッド (load/store/initial/final layout、render_targets と連動)
- attachment 名のオートコンプリート (Profile 内宣言済の attachment 名 + 予約名 swapchain)

## 9. KS 側の対応

KS は本書の対象外だが、ステップ D で `data/render/kuzu.profile.json` に attachments[] と attachment_ops[] を追加する PR を別建てする。Pictor 側のフォールバック (4.1 / 4.2) があるので KS 側 PR は Pictor merge の後追いで OK。

`spec/rendering_overview.md` に「Phase 3: scene-side 系統B」を追記 (KS 側 PR の中で)。

## 10. 実装スコープ見積もり

- Pictor 側: 新規ヘッダ 4 (`attachment_def.h` / `attachment_registry.h` / `render_pass_registry.h` / `framebuffer_registry.h`) + 実装 4 + 既存 `vulkan_context.cpp` / `render_pass_scheduler.cpp` / `pipeline_profile.h` / `pipeline_profile_serializer.cpp` の改修。新規 CTest 4 ファイル。
- Ergo 側: `profile_schema.ts` を v2 化 (attachments[] + attachment_ops[]) + Profile Editor UI に Attachments タブ + 各 pass の attachment_ops グリッド。
- KS 側: `data/render/kuzu.profile.json` の attachments[] 明示化 + `spec/rendering_overview.md` 追記 (別 PR)。

## 11. ブランチ + PR

- Pictor: `feat/pipeline-system-b` (本書 + Phase 3 実装一式、1 PR)
- Ergo: `feat/render-pipeline-system-b` (Profile Editor v2 + scanner v2 schema 認識、1 PR)
- KS: 後追いで別 PR
