---
task: fbx-track-playback
project: Pictor
kind: 実装
created: 2026-10-08
actio: actio:9ca6eef3-daf6-4e8a-98d9-55568d467c4b
source_session: lictor-e36b20ee-8a8e-4ddb-8d57-0f78253c8d54
memory_links:
  - spec/feature/fbx-track-playback.md
---
# FBX viewer: 外部トラック再生と raw フレーム出力

## 目的

`SPEC-PC-FBX-TRACK-PLAYBACK` を `demo/fbx_viewer` に実装する。ホストが CSV で
ボーン回転とブレンドシェイプ重みをフレーム単位に与え、描画結果を raw BGRA で
外部エンコーダへ流せるようにする (画像ファイルを経由しない長尺・決定的クリップ)。

## 設計 (責務ごとのファイル)

| ファイル | 責務 |
|---|---|
| `track_options.{h,cpp}` | 新 CLI オプションの解析と相互検証 |
| `track_csv.{h,cpp}` | CSV を起動時に 1 回だけ frame-major のフラット配列へ変換 |
| `track_rotation.h` | 度 → クォータニオン (内因性 X→Y→Z = `Rx*Ry*Rz`)、`bind_local * R` |
| `track_player.{h,cpp}` | フレーム毎に FK override 設定とモーフ重み適用 (毎フレーム確保なし) |
| `morph_targets.{h,cpp}` | 疎デルタ (slot + delta) と、元位置から再ベースする CPU 適用 |
| `morph_loader.{h,cpp}` | importer の `morph_target_names` / `morph_deltas` を packed mesh の頂点範囲へ対応付け |
| `swapchain_readback.{h,cpp}` | `FrameCapture` の read-back を、再利用 staging (常時 map) へ一般化 |
| `raw_frame_writer.{h,cpp}` | raw BGRA8 の書き出し (RGBA swapchain は行バッファで並べ替え) |
| `stdout_divert.{h,cpp}` | 実 stdout を機械出力専用に複製し、ログを stderr へ (`_setmode` で binary) |
| `channel_list.{h,cpp}` | `--list-channels` の JSON 出力 |

`main.cpp` はオプションの配線のみ。ブレンドシェイプは viewer cache に入っていないため、
`--track` / `--list-channels` では cache を読まず FBX を取り込む。

## 完了条件

- C-1 parse_track_csv(text, bones, shapes, out, error): ヘッダを bone 群 / morph チャンネルへ対応付け、欠けた軸は 0
- C-2 track_rotation_xyz_deg(rx, ry, rz): 度単位・内因性 X→Y→Z、`bind_local * R` で bone ローカル空間に合成
- C-3 parse_track_csv(...): frame の欠番・非数値・列数不一致・重複列・未知の列種別を拒否
- C-4 parse_track_csv(...): 未知の bone / shape 名は名前ごとに 1 回警告して無視、既知チャンネル 0 は エラー
- C-5 MorphApplier::apply(weights, positions, stride): 2 頂点 fixture で base + 0.5 × delta、繰り返しても蓄積しない
- C-6 pictor_fbx_viewer --list-channels: ウィンドウを開かず bone (親・bind head) と shape 名を JSON で stdout へ
- C-7 pictor_fbx_viewer --track --raw-out -: 全トラックフレームを BGRA8 で stdout へ書き、最後のフレーム後に終了

## 再利用探索

- `FrameCapture` の read-back: 採用 (一般化)。コピー命令とバリアを `SwapchainReadback` へ移し、
  BMP の one-shot と raw の毎フレーム出力が同じ経路を使う。staging は extent 単位で再利用。
- `AnimationSystem::set_fk_override`: 採用。クリップは再生せず bind pose + override で駆動。
- `SkinMeshDescriptor::morph_target_names` / `morph_deltas`: 採用。ロード時に疎リスト化。
- `pictor::float_parse::parse_double`: 採用 (CSV とオプションの数値解析、locale 非依存)。
- `pictor::Skeleton::compute_world_matrices`: 採用 (`--list-channels` の bind head 位置)。
- GPU 側でのモーフ適用 (シェーダ拡張): 不採用。仕様が CPU で host-visible VB へ書く方式を指定し、
  共有 shader / 頂点レイアウトを変えずに済む。

## 検証

- CTest: `unit_fbx_track_csv_test` (C-1〜C-4)、`unit_fbx_morph_targets_test` (C-5) — 通過。
- MSVC Debug: `pictor_fbx_viewer` / `pictor_pn_demo` / 新テストをビルド (新規ファイルに警告なし)。
- 手動: fbx/model1 で `--list-channels` (C-6)、60 フレーム CSV を `--raw-out -` → ffmpeg で
  640x720 / 60 フレームの mp4 (C-7)。口・目・首の追従をフレーム抽出で目視確認。
- GCC ビルドは未実施 (CI に委ねる)。POSIX 側は `dup` / `dup2` / `fdopen` を使用。

## 残作業 / 注意

- fbx/model1 のモデル空間は cm 相当 (Head の bind head y ≈ 130)。`--camera` はモデル空間の
  値をそのまま使う (仕様の「metres」はモデルの単位に従う)。
- raw 出力にも既定の bone overlay が描かれる。素の映像が要るときは `--no-bones` を併用する。
