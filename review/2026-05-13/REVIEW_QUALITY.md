# REVIEW_QUALITY.md — Pictor (2026-05-13)

評価: **B+**

C++20 + AVX2 + Vulkan。MSVC `/W4` + GCC/Clang `-Wall -Wextra` 有効 (`CMakeLists.txt:189-193`)。

## 良い点

- Q1. ヘッダ doc 豊富 (`rive_renderer.h:15-33` / `pictor_renderer.cpp:15-117`)。
- Q2. `FindRive.cmake:37-55` の lib_dir 探索や config 既定補完が現実的。
- Q3. demo の Vulkan / headless ガードで CI 適応度高。
- Q4. test 命名 (`unit_* / pixel_* / fps_*`) で層が一目瞭然。

## 指摘

- Q5. コメント番号不整合 (低)。`src/core/pictor_renderer.cpp:65 / :68` の `// 11.` 重複で以降ズレ。
- Q6. `fprintf(stderr, ...)` 散在 (低)。`src/vector/rive_renderer.cpp:112-197` 等。`set_log_handler` callback 推奨。
- Q7. placeholder 存続コスト (中, I1 関連)。`rive_animation.cpp` は silent-success stub。deprecate 推奨。
