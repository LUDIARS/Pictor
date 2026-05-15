# AUTOFIX.md — Pictor (2026-05-13)

本セッションは review-only。ソースコード修正は行わない。下記は将来的な自動修正候補の列挙のみ。

**autofix_count = 0**

## 自動修正候補 (列挙のみ)

1. `src/core/pictor_renderer.cpp:65-117` のステップ番号コメントのリナンバ (`// 11.` 重複の解消)。
2. `src/animation/rive_animation.h` の class / public 関数に `[[deprecated("Use pictor::RiveRenderer in <pictor/vector/rive_renderer.h>")]]` 付与。 (REVIEW_IMPLEMENTATION I1 / REVIEW_QUALITY Q7)
3. `.github/workflows/ci.yml` 新規追加 — `cmake -B build -DPICTOR_BUILD_DEMO=OFF -DPICTOR_BUILD_TESTS=ON && cmake --build build && ctest --test-dir build --output-on-failure` を Ubuntu + Windows matrix で実行。 (REVIEW_MISSING M1)
4. `CLAUDE.md` 新規追加 — 「上位ライブラリ非依存」「Rive 統合は cmake/FindRive.cmake が単一情報源」「Debug 実行が既定 (feedback_pictor_debug_default.md)」「exe 起動は user 側 (feedback_pictor_no_run.md)」を記載。 (REVIEW_MISSING M2)
5. `plan.md` → `DESIGN.md` リネーム + README からの参照更新。 (REVIEW_MISSING M2)
6. `src/vector/rive_renderer.cpp:148-158` の `shutdown()` 内で `impl_->image_access.clear()` 追加 + render target 再作成時 (`:302-314`) にも clear。 (REVIEW_IMPLEMENTATION I3)
7. `src/vector/rive_renderer.cpp:162-178` `load_riv_file` に `MAX_RIV_BYTES` (例 64 MiB) の上限チェック追加。 同様に `src/animation/rive_animation.cpp:25-42`。 (REVIEW_VULNERABILITY V1)
8. `include/pictor/vector/rive_renderer.h` に `enum class RiveError` + `RiveError last_error() const` を追加し、 `fprintf(stderr, ...)` 由来の診断を構造化する。 (REVIEW_DESIGN D8 / REVIEW_QUALITY Q6)
9. `include/pictor/surface/vulkan_context.h` (要確認) に `const std::vector<VkImage>& swapchain_images() const` 追加 → `demo/rive/main.cpp:178-188` の毎フレーム `vkGetSwapchainImagesKHR` 2 回呼びを削除。 (REVIEW_IMPLEMENTATION I2)

いずれもソース修正を伴うため、 別 PR / 別セッションで実施する。
