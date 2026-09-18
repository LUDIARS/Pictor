---
task: xr-hmd-verification
project: Pictor
kind: テスト
created: 2026-09-19
memory_links:
  - C:/Users/raury/.claude/projects/E--Document-Ars/memory/project-pictor-xr-vr-module.md
---
# XR モジュールをヘッドセットで確かめる

## 目的

`Pictor::xr` (spec/feature/vr-openxr-design.md) は build と単体テストまでで、HMD での表示を確かめていない。
再投影の見た目、実際の穴の比率、GPU 負荷の削減量が未確認のまま v2.2.0 の次のコミットとして公開されている。

## 完了条件

- `-DPICTOR_ENABLE_OPENXR=ON` で build した `pictor_vr_openxr_demo` を Quest Link で起動し、部屋を見渡せることを確認する (実行はユーザー側)。
- preset 4 種で、300 フレームごとに出る穴の比率と、目視での破綻 (輪郭のにじみ・穴の残り・動く箱の欠け) を記録する。
- `vr.two_pass` を基準に、`vr.eye_delta` / `vr.film_delta` の GPU 時間の差を記録する。
- Vulkan の validation layer を有効にして起動し、エラーが出ないことを確かめる。
- 結果を spec/feature/vr-openxr-design.md の「確認できていること / いないこと」へ反映する。
- Revisor のマージ用リポジトリと本体に残る旧履歴のタグ (v2.2.0 以外) が、次のリリースで同名衝突しないかを確認する。

## スコープ (編集可ディレクトリ)

- `spec/feature/`
- `demo/vr_openxr/`
- `src/xr/`、`shaders/xr/` (不具合が見つかった場合)
