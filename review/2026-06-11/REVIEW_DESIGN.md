# 設計レビュー — Pictor (2026-06-11, Fable 診断)

規約基準: `CLAUDE.md` (DoD hot path / OOP 境界 / SOLID 全 5 / 最下層レイヤ)。
前回: `review/2026-05-30/CODE_CONVENTIONS_REVIEW.md`。

## サマリ

- **前回指摘 (2026-05-30) の修正状況: 9 件中 0 件修正**。God Class / Profiler string / SkinnedVertex alignment すべて残存。
- 新規の筆頭リスクは「**managed パス (PictorRenderer 内蔵) と host-driven パス (CompiledGraph) の二重アーキテクチャ**」。managed 側 GPU 系は大部分がスタブで、統計 API が虚偽値を返す。
- 一方、**pipeline サブシステム再設計 (PipelineCompiler / 3 レジストリ / CompiledGraph) は DoD 思想が明文化された良設計**。ヘッダ循環ゼロ・上位ライブラリ実依存ゼロも機械検証済み。

## 前回指摘の追跡

| 前回指摘 (2026-05-30) | 状態 | 証拠 |
|---|---|---|
| PictorRenderer God Class (18 unique_ptr) | **未修正** | `include/pictor/core/pictor_renderer.h:262-279` |
| `initialize()` 18 ステップ単一関数 | **未修正** | `src/core/pictor_renderer.cpp:13-100` |
| Profiler per-frame string 比較 | **未修正** | `src/profiler/profiler.cpp:37-44, 85-101` |
| SkinnedVertex `alignas` なし | **未修正** (ただし下記 M-5: 前回提案自体が実施不能) | `include/pictor/animation/skinned_vertex.h:25-32` |
| `pipeline_builder` の unordered_map 受け API | **未修正** | `src/pipeline/pipeline_builder.cpp:109,239,254` |
| SSBOLayout public struct + alloc 同居 | **未修正** | `include/pictor/gpu/gpu_buffer_manager.h:20-40` |
| pictor_renderer.cpp 669 行 (責務混在) | **未修正** (今も 669 行) | - |
| Profiler 3 分割 | **未修正** | `include/pictor/profiler/profiler.h` |
| 「Ergo-web」等の上位名コメント | **残存・増加** | `pictor_renderer.h:109`, `decal_system.h:41`, `ui_renderer.h:3-7` 他 |

---

## High

### D-1. PictorRenderer God Class 未解消 (前回 Critical 継続)
**`include/pictor/core/pictor_renderer.h:262-279` / `src/core/pictor_renderer.cpp:13-100`**

18 サブシステムの `unique_ptr` 所有 + lifecycle / frame / object / profile / mobile / profiler / overlay / animation / data / GI / bake / export の 12 責務群 (~60 public メソッド)。初期化順序の依存がコメントでしか守られておらず、`apply_profile` (cpp:438-483) がサブシステム生成/破棄まで行うため変更影響が全域に及ぶ。
**方向**: `RendererSubsystemManager` (所有 + 構築順序) / `MobileLifecycleController` (cpp:336-436 の ~100 行) / `GIFacade` (cpp:587-645) の 3 切り出しが最小コスト。公開 facade の委譲メソッドはそのまま残せる。

### D-2. managed フレームループの大部分がスタブで、統計が虚偽になる
- `src/pipeline/render_pass_scheduler.cpp:39-98` — `execute()` の PassType switch は SHADOW 以外すべて空 case。SHADOW も `(void)shadow_batches;` (72 行) で結果を捨てる。
- `src/gpu/gpu_driven_pipeline.cpp:52-80` — 「Actual Vulkan dispatch calls would go here」のまま。`stats_.visible_objects = object_count; // placeholder` (78 行)。
- `src/profiler/overlay_renderer.cpp:10-15, 46-50` — 「In a real implementation:」コメントのみ。
- `CommandEncoder::encode()` は**リポジトリ内に呼出箇所ゼロ** (grep 検証)。にもかかわらず `pictor_renderer.cpp:213-214` が `command_encoder_->draw_call_count()` を profiler に記録 → **draw call / triangle 統計は常に 0**。

`get_frame_stats()` という公開 API が虚偽の値を返すのは契約違反。実描画は host-driven (`execute_compiled` / SimpleRenderer / PostProcessPipeline) に移行済みなのに、旧経路が「動いているように見える」形で残っている。
**方向**: (a) 旧 `execute()` / CommandEncoder / OverlayRenderer スタブを削除し CompiledGraph 経路へ一本化 (spec の Phase 進行から見て本筋)、または (b) `is_simulated()` 等で明示隔離し統計 API から虚偽値を排除。

### D-3. Profiler / GpuTimer の per-frame string 操作 (前回 High 継続)
**`src/profiler/profiler.cpp:85-101, 37-44` / `include/pictor/profiler/gpu_timer.h:72-117` / `src/profiler/gpu_timer.cpp:113-159`**

`begin_cpu_section(const std::string&)` が毎フレーム string 入り struct を push_back、終了側は string 線形比較。`render()` 1 回あたり約 10 区間 × begin/end。GpuTimer も `name + "_begin"` の**毎フレーム文字列連結 (ヒープ確保)**。CLAUDE.md「frame 内でコピー / 再 alloc しない」違反であり、`compiled_graph.h` が掲げる「hot path に string ゼロ」不変条件と同一コードベース内で矛盾。
**方向**: `enum class ProfileSection : uint8_t` + `std::array<Timer, kCount>`。名前は表示時のみ `constexpr const char*` テーブル引き。

## Medium

### M-1. WorldPartition: per-frame の unordered_map 操作 (新規・DoD 違反)
**`include/pictor/culling/world_partition.h:95-98` / `src/culling/world_partition.cpp:37-71, 103-110`**

`cells_` / `object_cell_map_` が `unordered_map` で、`culling_system.cpp:64` のコメントが毎フレーム `assign_object` 運用を明記。`query_frustum` は毎フレーム map 全走査。broad phase の意味が pointer chase で相殺される。partition は opt-in のため Medium。
**方向**: 固定分割 grid なので flat array (`divisions³`) + `object_to_cell` flat vector に置換可能。

### M-2. remap_batches_for_pass の per-frame vector 確保
**`src/pipeline/render_pass_scheduler.cpp:172-203`** — pass ごとに `std::vector<RenderBatch>` を新規確保。`FrameAllocator` を引き回しているのに未使用。D-2 で旧経路を消すなら同時に消滅。

### M-3. PipelineCompiler の framebuffer 解決が「位置」依存 (潜在バグ)
**`src/pipeline/pipeline_compiler.cpp:157 vs 176`**

VkRenderPass は `rps.index_of(pd.pass_name)` の名前解決なのに、framebuffer は profile 配列の位置 `i` で `fbs.get(i, s)`。registry へ渡した pass リストと compile 対象 profile の順序一致という暗黙契約があり、並べ替え・部分プロファイルで render_pass と framebuffer が食い違う。失敗時も silent (VK_NULL_HANDLE → skip)。
**方向**: `fbs.get(rps.index_of(pd.pass_name), s)` に統一し、不一致は compile 時エラーに。あわせて descriptor pool の先行生成 (137-143 行、set は未 allocate で `input_sets` 全 NULL) は Phase 4 実装まで遅延すべき。

### M-4. umbrella ヘッダが Vulkan / GLFW を全 consumer に伝播 (DIP)
**`include/pictor/pictor.h:64-65`** が `vulkan_context.h` / `glfw_surface_provider.h` を無条件 include → `<vulkan/vulkan.h>` が全 consumer に伝播。公開ヘッダ 16 ファイルが vulkan.h を直 include。pipeline 系の host-driven 契約による意図的露出は妥当だが、抽象 API だけ使う consumer の入口 (`pictor.h`) からは外し、`pictor/pictor_vulkan.h` 等の opt-in umbrella に分離すべき。
良い点: `surface_provider.h` は純抽象で GLFW ヘッダ非依存 (DIP 良好)。

### M-5. SkinnedVertex alignment — 前回提案は実施不能、バッファ側対応が正解
**`include/pictor/animation/skinned_vertex.h:25-32`** — `static_assert(sizeof == 56)` が `lit.vert` の interleaved layout と結合しており、前回提案の `alignas(64)` は GPU layout を壊す。**修正記録**: struct でなく `pack_skinned_vertices()` の確保を aligned allocator / FrameAllocator に変えるか、SIMD 用途には SoA を別途持つ。

### M-6. 巨大ファイル / 責務同居
実測上位: `demo/fbx_viewer/main.cpp` 2102 行、`src/pipeline/pipeline_profile_serializer.cpp` **1443 行** (JSON tokenizer + enum⇔string 変換 + serializer + deserializer の 4 責務同居 — tokenizer と変換テーブルは独立ファイルに切れる)、`src/animation/fbx_scene.cpp` 1220 行、`fbx_document.cpp` 993 行。

## Low

- **L-1**: 上位ライブラリ名 (KuzuSurvivors / ergo_ui_kit / kuzu.profile.json) のコメント・文字列が前回から**増加** (`pictor_renderer.cpp:97`, `compiled_graph.h:76`, `tests/unit_visus_serializer_test.cpp:14` 等)。実依存ゼロは維持。「host」「上位エディタ」への言い換え推奨。
- **L-2**: `object_pool.h:6` の `#include <unordered_map>` 未使用 (id_map 移動済みの残骸)。
- **L-3**: `src/c_api/c_api.cpp:37-38` — `unordered_map` + mutex の per-call 台帳だが `pictor_render_frame` は no-op で SceneRegistry 未接続。Phase 2 で二重台帳を作らないこと。なお **c_api.cpp はどのビルドターゲットにも含まれていない** (REVIEW_QUALITY.md 参照)。
- **L-4**: `command_encoder.cpp:5-7` — `encode()` の `FrameAllocator&` 引数が未使用。
- **L-5**: `render_pass_scheduler.cpp:29-31` — custom pass 解決が毎フレーム string 比較。reconfigure 時に index 化可能。

---

## pipeline サブシステム再設計の評価 (Phase 3/4 系統B 解体)

**結論: 新設計そのものは一貫しており責務分離は良好。課題は旧経路の残置と縫い目 (D-2, M-3)。**

- 3 レジストリの責務が明確: AttachmentRegistry (名前→uint16 + VkImage 所有)、RenderPassRegistry (VkRenderPass)、FramebufferRegistry ((pass, slot) flat 配列)。所有権コメント (`pipeline_compiler.h:35-39`) が明文。
- `CompiledGraph` は hot-path 不変条件 (string/map ゼロ、VkHandle 直値、debug_name は static literal) を**ヘッダに明記** (`compiled_graph.h:11-14`) し、`execute_compiled` (`render_pass_scheduler.cpp:104-168`) は宣言どおり flat seq-iterate + 1 分岐。DoD 規約のお手本。
- `PICTOR_HAS_VULKAN` ガードの層分けで headless ユニットテスト可能 (`unit_pipeline_registries_test.cpp` で実証)。
- 不足: `PipelineCompiler` を呼ぶコードが repo 内になく (host 側のみ)、compile→execute_compiled の統合テストが存在しない。headless で graph 構造の compile 結果を検証するテスト追加を推奨。

## 強み (genuine strengths)

1. **DoD 中核は本物**: ObjectPool 完全 SoA + hot/cold 分離、swap-and-pop、BatchBuilder/CullingSystem の per-frame 確保は全て FrameAllocator 経由、`float4x4` は `PICTOR_CACHE_ALIGN`、`BVHNode` は 32B/`alignas(32)` + static_assert。
2. **UpdateScheduler**: 戦略選択 + AVX2 NT store + prev_transform の memcpy — 規約の理想形 (NT store の実装不備は REVIEW_IMPLEMENTATION.md L-3 参照)。
3. **ISP/OCP**: ICullingProvider / IBatchPolicy / IUpdateCallback / IJobDispatcher / ICustomRenderPass / ISurfaceProvider / IMobileLifecycleObserver の細粒度分離。MaterialRegistry は handle 直 index の flat vector O(1)、kind は registry 登録で switch 増殖なし (kind switch は serializer / Vk 変換の init 時に限定)。
4. **レイヤ健全性**: include 循環 0、上位実依存 0、依存は Vulkan/GLFW/標準ライブラリのみ。
5. **ビルド規約遵守**: test target への `/utf-8` 個別指定済み。

## 推奨アクション順

1. **D-2**: 旧 managed 経路の削除 or 隔離 — 虚偽統計の解消が最優先
2. **D-1**: PictorRenderer 3 分割 (SubsystemManager / MobileLifecycle / GIFacade)
3. **D-3**: Profiler の enum 化 — 差分が小さく即効
4. **M-3**: PipelineCompiler の名前ベース統一 + headless compile テスト
5. **M-1**: WorldPartition の flat grid 化 (本格利用開始前に)
