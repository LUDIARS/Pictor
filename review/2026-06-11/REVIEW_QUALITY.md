# 品質保証レビュー — Pictor (2026-06-11, Fable 診断)

対象: Vulkan 実装の正しさ / エラー処理戦略 / テストカバレッジ / プラットフォーム実装の実在性 / ロギング方針。
全指摘は実コード読解で検証済み (HEAD: 4789398)。

---

## 1. Vulkan 正しさ

### High

**[Q-1] acquire 失敗時の fence デッドロック** — `src/surface/vulkan_context.cpp:107-118`
`acquire_next_image()` は fence wait → **reset してから** `vkAcquireNextImageKHR`。`VK_ERROR_OUT_OF_DATE_KHR` で `UINT32_MAX` を返すとその frame は submit されず fence は unsignaled のまま → 次フレームの `vkWaitForFences(..., UINT64_MAX)` が**永久ブロック**。リサイズ中に再現し得る。
**修正**: fence reset を submit 確定後に移すか、失敗パスで re-signal。

**[Q-2] iOS サーフェスが作成不能 (実質スタブ)** — `vulkan_context.cpp:202-289` / `CMakeLists.txt:233`
CMake は iOS で `VK_USE_PLATFORM_METAL_EXT` を定義し `IOSSurfaceProvider` は `Type::iOS` と `VK_EXT_metal_surface` を返すが、`create_surface()` の switch に **iOS / `vkCreateMetalSurfaceEXT` の case が存在しない** (あるのは macOS MVK の Cocoa のみ)。iOS ビルドは必ず "Unsupported platform surface type 7" で初期化失敗。コンパイルは通るため気づきにくい。

**[Q-3] frames-in-flight 1 固定 + semaphore 再利用** — `vulkan_context.cpp:679-694` / `vulkan_context.h:148-150`
fence 1 本・semaphore 1 組のみ。(a) CPU/GPU 直列化で性能上限、(b) `render_finished_sem_` を全 swapchain image で共有 (最新 validation layer で VUID エラーになる既知パターン)。`with_flight_count` (`pipeline_builder.cpp:181`) や `compiled_graph.h:43` の `input_sets[4]` は multi-flight 前提で、コンテキストとの設計不整合。

### Medium

- **[Q-M1] validation layer 存在未確認** — `vulkan_context.cpp:160-162`: SDK 未導入マシンで `VK_ERROR_LAYER_NOT_PRESENT` → 初期化ごと失敗。`vkEnumerateInstanceLayerProperties` で確認すべき。
- **[Q-M2] initialize() 途中失敗でリーク** — `vulkan_context.cpp:34-61`: `initialized_` が立たないと dtor の `shutdown()` も走らず instance/device/surface がリーク。`SimpleRenderer::initialize` (`simple_renderer.cpp:28-41`) も同パターン。
- **[Q-M3] acquire/present の他の VkResult 無視** — `vulkan_context.cpp:111-135`: `DEVICE_LOST`/`SURFACE_LOST` で index=0 のまま成功扱い。acquire 側 `VK_SUBOPTIMAL_KHR` 未処理。`vkWaitForFences`/`vkResetFences` 戻り値未チェック。
- **[Q-M4] swapchain 再作成時に command buffer 数を更新しない** — `vulkan_context.cpp:92-104` vs 663 行。image 数変化で不整合。
- **[Q-M5] surface format 0 件で UB** — `vulkan_context.cpp:482`: `formats[0]` 無条件参照。capabilities 系の戻り値も全て未チェック (469-479 行)。
- **[Q-M6] SimpleRenderer エラーパスのリーク** — `simple_renderer.cpp:166-168` (vert 成功/frag 失敗で shader module リーク)、388-400 行 (alloc 失敗時 VkBuffer 未破棄、`vkBindBufferMemory` 未チェック)、`vkMapMemory` 未チェック 4 箇所 (73, 93, 420, 430 行)。

### Low
- ハードコード上限: `ext_names[16]` (`vulkan_context.cpp:154`)、`MAX_INSTANCES = 1,100,000` (SSBO ~17.6MB 常時確保、`simple_renderer.h:82`)。
- `end_single_time_commands` (`vulkan_context.cpp:745-757`): submit 未チェック + 毎回 `vkQueueWaitIdle`。
- `gpu_timer.cpp:198-227`: `DEVICE_LOST` 継続時 `rf.pending` が永久 true (実害は古い値の据え置きのみ)。
- `render_pass_scheduler.cpp:143-147`: RP/FB 欠落時に record callback を呼ぶ fallback — ホスト誤認で validation エラーの温床 (コメント明示済み)。

### 良い点 (Vulkan)
- `shutdown()` 冒頭の `vkDeviceWaitIdle` は正しい (`vulkan_context.cpp:66`, `simple_renderer.cpp:45`)。
- features2 / pNext チェーン (interlock / ROV probe、`vulkan_context.cpp:387-453`) は VUID を意識した丁寧な実装。
- GpuTimer の flight ローテーション + 非ブロッキング回収、timestampPeriod=0 GPU でのシミュレーション継続 (`gpu_timer.cpp:44-49`)。

---

## 2. エラー処理戦略

基本方針は「bool 戻り値 + fprintf(stderr)」で**ほぼ一貫**。例外は allocator 2 箇所 (`frame_allocator.cpp:59`, `pool_allocator.cpp:106` の `bad_alloc`) のみで境界明確。ただし:

- **[M] 黙殺 catch**: `src/animation/lottie_animation.cpp:94` — `catch (...) {}`。marker の `tm` パース失敗が黙って 0 になる (96-106 行の `dr` 側はフォールバックありで対照的)。
- **[M] 未実装の silent no-op**: `render_pass_scheduler.cpp:39-98` — 呼んでも何も描かれないが失敗も報告されない (詳細は REVIEW_DESIGN.md D-2)。
- **[M] ビルドから漏れた orphan ソース**: **`src/c_api/c_api.cpp` (161 行) が `CMakeLists.txt` のどのターゲットにも含まれていない**。コンパイルすらされない死蔵コード。ビルドに載せるか削除を。
- **[L] 設定キーの typo 黙殺**: `pipeline_builder.cpp:252-334` — 未知キー無視 + parse 失敗 silent fallback。`parse_uint` は負数/オーバーフロー未検出 (26-32 行)。`parse_shader_handle` が `INVALID_MESH` を ShaderHandle の無効値に流用 (72-83 行、型混同)。
- VkResult 未チェック群 (上記 Q-M3/M5/M6) も同カテゴリ。`VK_CHECK` 的ヘルパー導入で一括改善可能。

---

## 3. テストカバレッジ

登録方式: `tests/CMakeLists.txt:11-37` — 独立 exe + CTest 直登録、**全テスト headless** (Vulkan/GLFW 不要、明記+実装とも整合)、**MSVC `/utf-8` per-target 設定済み** ([[feedback_msvc_utf8_test_targets]] 準拠)、タイムアウト 60s。基盤は良好。

| サブシステム | 単体テスト | 備考 |
|---|---|---|
| culling / visus / shader | ○ | |
| memory | △ | frame_allocator のみ。pool / gpu_memory_allocator なし |
| batch | △ | radix_sort のみ。batch_builder なし |
| pipeline | △ | serializer / attachment_def / registries のみ。**scheduler / command_encoder / compiler / builder はゼロ** |
| postprocess / text | △ | chain / rasterizer 経路のみ |
| core / scene / update | ✕ | fps_baseline が間接カバー |
| **animation (14 ファイル)** | **✕** | **FBX/BVH/Lottie パーサ含む — 外部入力をパースする最も壊れやすい層が空白** |
| data / material / gpu / gi / decal / ui / profiler / surface / vector / webgl / c_api | ✕ | ゼロ |

**最優先で足すべき**: (1) animation importer 群 — REVIEW_VULNERABILITY.md の V-4〜V-7 の再現入力をそのまま回帰テスト化できる、(2) batch_builder — REVIEW_IMPLEMENTATION.md H-1 (static バッチ消失) は 10 行のテストで捕まえられた、(3) pipeline_builder の kv パースと headless compile。

---

## 4. プラットフォーム実装の実在性

- **src/webgl/** (計 1,092 行): **実実装** (context 作成・shader compile・instanced draw まで本物)。`#ifdef PICTOR_HAS_WEBGL` で非 Emscripten でも空 TU としてコンパイル可、CMake 整合。ただし **`glGetError` チェックが全ファイルで 0 件** — GL エラー完全黙殺 (Medium)。
- **android_surface_provider**: 実装として完結。`vulkan_context.cpp:272-283` の case・CMake とも整合。
- **ios_surface_provider**: provider 自体は完結だが受け側欠落 ([Q-2]) で機能としてはスタブ同然。

---

## 5. ロギング / 診断

- **良い**: `rive_renderer.cpp:11-15` の `RIVE_DBG` (NDEBUG ゲート)、`stats_overlay.cpp:109-111` の `PICTOR_STATS_DEBUG` — [[feedback_pictor_debug_default]] の方針に合致。
- **per-frame spam なし** (検証済み): printf/fprintf は init 時 or エラー時のみ。hot path にコンソール出力なし。
- **[L] 方針とのズレ**: init ログ (`vulkan_context.cpp:322`、`gpu_timer.cpp:73`、`simple_renderer.cpp:38` 等) は Release でも無条件 printf。1 回きりで実害は薄いが `PICTOR_LOG_INFO` 的共通マクロへの統一を推奨。

## 総評

骨格 (swapchain 基本フロー、features2 チェーン、headless テスト基盤、Debug ゲート方針) は丁寧。主リスクは **Q-1 (fence デッドロック) / Q-2 (iOS 動作不能) / Q-3 (1-flight 同期設計)**。エラー処理は方針一貫だが VkResult 未チェックとエラーパスのリーク掃除が系統的に漏れており、`VK_CHECK` ヘルパーで一括改善できる。テストは「外部入力パーサ」と「pipeline 中核」が空白地帯で、今回の脆弱性・バグ指摘の多くは単体テストがあれば設計時に捕まえられた類のもの。
