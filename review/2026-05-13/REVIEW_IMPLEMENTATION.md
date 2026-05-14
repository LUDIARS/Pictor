# REVIEW_IMPLEMENTATION.md — Pictor (2026-05-13)

評価: **B+**

74 cpp + 84 header で広範囲カバー。placeholder と本実装の同居が課題。

## 指摘

- I1. Rive 二重実装 (中)。`rive_animation.cpp:50-175` は Default artboard + `memset 0` の stub。deprecate or 削除。
- I2. demo 側 swapchain 解決 (低)。`demo/rive/main.cpp:178-188` が毎フレーム `vkGetSwapchainImagesKHR` 2 回呼出。`VulkanContext::swapchain_images()` で 1 行化。
- I3. `image_access` map TTL なし (低)。`rive_renderer.cpp:64 / 320 / 378-385` が recreate 後の旧ハンドルを保持。`shutdown()` / RT 再作成時に `clear()`。
- I4. ステップ番号重複 (低)。`pictor_renderer.cpp:65 / :68` の `// 11.` 同番号で以降ズレ。
- I5. `final_layout` センチネル (低)。`:359-360` の `UNDEFINED = 省略` は header 明示済だが、fence/semaphore 責務も追記したい。

## 良い点

- CMake オプション 7 軸で粒度分離、`FindRive.cmake:108-115` の自己解決メッセージ、headless demo + CTest (`tests/CMakeLists.txt:11-37`) で CI smoke run 適合。
