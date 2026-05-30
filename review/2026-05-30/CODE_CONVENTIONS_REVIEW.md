# Pictor コード規約レビュー (2026-05-30)

規約: `AIFormat/RULE_CODE.md` (共通) + `Pictor/CLAUDE.md` 「コード規約 (Pictor 固有)」 — DoD (cache-line aligned + flat array + hot path scatter 禁止) / OOP 境界 / SOLID 全 5 / Pictor は最下層 (上位 import 禁止)。

## サマリ
- 違反件数: Critical 1 / High 3 / Medium 4 / Low 2
- 注目点: `PictorRenderer` の責務複合 (18 unique_ptr) が筆頭。 alignment 指定の欠落 (`SkinnedVertex`) と Profiler の string-key 検索 (per-frame) が hot path 周辺の主リスク。

## Critical

- `include/pictor/core/pictor_renderer.h:53` — `PictorRenderer` が 18 個の `unique_ptr` でほぼ全サブシステムを所有
  - 違反: 共通 §1 (SRP / God Class 禁止) + Pictor 固有 SOLID-S
  - 詳細: 初期化順序 / teardown / モバイルライフサイクル / プロファイル適用 / GI / アニメーション切替 ... を 1 class が全部抱える。
  - 修正提案: `PictorRenderer` を「公開 facade」 だけにし、 `RendererSubsystemManager` (= ownership) と `LifecycleCoordinator` (= init / shutdown / suspend) を切り出す。 frame loop も `FrameLoop` 専用クラスへ。

## High

- `src/core/pictor_renderer.cpp:1-100` — `initialize()` が 18 ステップ単一関数
  - 違反: 共通 §「制御フロー / 早期リターン」 + 上記 SRP の連鎖
  - 修正提案: `init_memory_tier()` / `init_render_tier()` / `init_pipeline_profile()` / `apply_runtime_profile()` 等の private helper に段階分割。

- `include/pictor/animation/skinned_vertex.h:25-30` — `SkinnedVertex` (float[3] + uint32[4] 等) に `alignas` 指定なし
  - 違反: Pictor 固有 「SIMD 前提構造体は `alignas(16)` 以上、 cache line 跨ぎは `alignas(64)`」
  - 詳細: `sizeof = 56` bytes で cache line 未満だが、 hot path で `pack_skinned_vertices()` が SoA pack するため alignment 明示が必要。
  - 修正提案: `struct alignas(64) SkinnedVertex { ... };` または SoA flat buffer (position[] / weights[] 分離) に変更。

- `src/profiler/profiler.cpp:37-53` — per-frame で section 名 (`std::string`) を if 等価比較
  - 違反: Pictor 固有 「frame 内でコピー / 再 alloc しない」
  - 詳細: frame ごとに `std::string` の比較・潜在的なコピーが走る。 hot path 直近で string 操作は意図せぬ alloc を生む。
  - 修正提案: `ProfileSection` enum を導入し、 `std::array<Timer, kSectionCount>` で配列化。 string は表示時のみ map 引き。

## Medium

- `include/pictor/pipeline/pipeline_builder.h:6` — `PipelineProfileBuilder::from_key_value()` が `std::unordered_map` を直接受ける
  - 違反: Pictor 固有 「hot path に map を増やさない」 (= 境界 API としてリスク)
  - 詳細: 初期化のみで使われるが、 consumer が runtime に呼ぶ可能性が API 上開いている。
  - 修正提案: 関数コメントで `// initialization-only; do not call per-frame` を明示、 または builder method (`.set("key","value")`) を主 API にして map 引き渡しは internal-only に。

- `include/pictor/gpu/gpu_buffer_manager.h:21-31` — `SSBOLayout` が public フィールド struct + GPUBufferManager で alloc/dealloc/resize
  - 違反: 共通 §1 (データ責務とメモリ管理の同居)
  - 修正提案: `SSBOLayout` は値 (PoolHandle) に格下げ、 public API は `allocate_soa_buffers(count) → PoolHandle` に統一。

- `src/core/pictor_renderer.cpp` (669 行) — 単一 file に init / shutdown / lifecycle / frame loop が同居
  - 違反: 共通 §2 (ファイル分割) — 「責務単位」 で見ると複数。
  - 修正提案: `pictor_renderer_init.cpp` / `pictor_renderer_lifecycle.cpp` / `pictor_renderer_frame.cpp` 等に分割。 行数は基準ではなく「責務単位」 で。

- `include/pictor/profiler/profiler.h` — Profiler が timing + stats 集計 + overlay state を混在
  - 違反: 共通 §1 (SRP)
  - 修正提案: `Profiler` (measurement only) / `ProfilerStatsAggregator` (formatting) / `ProfilerOverlay` (UI state) に 3 分割。

## Low / その他観察

- `include/pictor/surface/glfw_surface_provider.h` — GLFW への依存は `ISurfaceProvider` 抽象に閉じており DIP 良好。 consumer (`demo/main.cpp`) も Vulkan handle / GLFW window を直接触っていない (= 規約遵守)。
- `include/pictor/core/pictor_renderer.h:105-109` — コメント内に「Ergo-web editor」 への言及あり (= 上位ライブラリ参照)。 「runtime tooling」 等に汎化して上位名を消すと、 規約「上位 import 禁止」 と整合。

## 全体評価

- **DoD 基盤**: `PICTOR_CACHE_ALIGN` macro / frame allocator / per-frame reset 設計は堅牢。
- **interface 抽象**: `ISurfaceProvider` / `IBatchPolicy` / `ICustomRenderPass` で層分離良好。
- **筆頭リスク**: `PictorRenderer` の God Class 化と `SkinnedVertex` の alignment 欠落。 ここを直すと SOLID 5 観点 + DoD 観点が同時に上がる。
