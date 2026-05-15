# REVIEW_DESIGN.md — Pictor (2026-05-13)

評価: **A-**
## 良い点

- D1. 3 層 SoA パイプライン (`plan.md:45-79`) を `PictorRenderer` (`src/core/pictor_renderer.cpp:12-235`) で実現。
- D2. Rive 境界良好。header の「Rive 型 public 非露出」明示 + `#ifdef PICTOR_HAS_RIVE` + Pimpl。
- D3. `cmake/FindRive.cmake:42-77` が premake 名揺れと出力レイアウト 2 系統に両対応。
- D4. 上位非依存遵守。272 ファイル走査で ergo / adventurecube / synergos / memoria / cernere / actio / excubitor 参照 0。

## 懸念

- D5. Rive 二重実装 (中)。`rive_animation.cpp:7-176` は Default placeholder、`rive_renderer.cpp` 本実装と並存。前者を deprecate。
- D6. `plan.md` (817 行) が DESIGN を兼ね、LUDIARS 標準 `DESIGN.md + spec/` から外れる。
- D7. demo `add_executable` 16 個が `CMakeLists.txt:199-288` 直書き。`add_pictor_demo()` 関数化推奨。
- D8. `PICTOR_HAS_RIVE` 未定義時 fallback が「静かに false」(`:111-179`)。`last_error()` API 推奨。
