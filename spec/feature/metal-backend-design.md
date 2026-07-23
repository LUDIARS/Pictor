# Metal Backend — iOS 直呼び設計 (RHI 合流 / SHaRC 先行)

> ステータス: **設計 + 移植マッピング** (実装は macOS + 実機環境が前提)。
> iOS は現状 MoltenVK 前提 (`portability/mobile.md` §1) だが、 本書は
> **Metal 直呼び** へ置き換える。 Android は既存の Vulkan 直呼びを維持する。
> RHI 抽象は `dx12-backend-design.md` の `IRhi*` 案に合流する (DX12 /
> WebGPU / Metal の分岐点は 1 つ)。

## 1. 目的

iPhone 15 以降 (A17 Pro / Apple GPU) で Pictor — 特に SHaRC 拡張
ライティングキャッシュ (`shaders/sharc/`) — を **Metal 直呼びで動かす**。

MoltenVK を外す理由:

- 翻訳レイヤのオーバーヘッドとデバッグ困難 (Metal Frame Capture が
  SPIR-V 変換後の MSL しか見えない)
- App Store 審査・OS 更新への追従安定性
- Metal 固有機能への到達余地 (tile shading / MetalFX upscaling /
  mesh shaders)

## 2. スコープと段階

**Phase M (本書)**: SHaRC compute パイプラインの Metal 実装を先行する。

理由: SHaRC は API 表面が極端に薄い —
compute パス 5 本 + fullscreen fragment 1 本 + SSBO 群 + indirect dispatch
のみで、 レンダパスグラフ / マテリアルシステム / スワップチェイン管理の
本体側複雑性に依存しない。 RHI 全面改修 (Phase R) を待たずに実機検証へ
到達できる。

**Phase R (別途)**: 本体レンダラの RHI 化 (`dx12-backend-design.md` §3 の
`IRhiDevice` / `IRhiCommandEncoder` / `IRhiBuffer` ...) と、 その Metal
実装。 Phase M の Metal コードはそのまま Phase R の実装素体になる。

**非目標**: DoD コア (scene/batch/culling) の変更。 SHaRC のセル表現 /
パス構成の変更 (アルゴリズムは API 非依存)。

## 3. リソース / コマンドのマッピング表

`SharcGpuExecutor` (`src/gi/sharc_executor.cpp`) の Vulkan 使用箇所と
Metal 対応物。 executor と同一の公開 API を持つ `SharcMetalExecutor`
(Objective-C++) を実装する。

| Vulkan | Metal | 備考 |
|---|---|---|
| `VkBuffer` + `VkDeviceMemory` (device-local) | `MTLBuffer` (`storageModePrivate`) | 初期化ゼロクリアは blit `fillBuffer` |
| 〃 (host-visible mapped) | `MTLBuffer` (`storageModeShared`) | **UMA なので staging 層自体が不要** — rays/shade staging と本体を 1 本に統合できる |
| `vkCmdCopyBuffer` (staging→device) | 不要 (Shared) / `MTLBlitCommandEncoder copyFromBuffer` | シーン転送も Shared で可 (帯域は同一物理メモリ) |
| `vkCmdFillBuffer` | `MTLBlitCommandEncoder fillBuffer` | counters クリア |
| `VkComputePipeline` (SPIR-V) | `MTLComputePipelineState` (MSL) | シェーダは §4 で移植 |
| `vkCmdBindDescriptorSets` (binding N) | `setBuffer:offset:atIndex:N` | binding 番号をそのまま index に流用 |
| UBO (`SharcParams`) | `constant SharcParams&` (`setBuffer` or `setBytes`) | 128B なので `setBytes` で毎フレーム直渡し可 |
| `vkCmdDispatch(x,1,1)` | `dispatchThreadgroups:MTLSizeMake(x,1,1) threadsPerThreadgroup:{64,1,1}` | |
| `vkCmdDispatchIndirect` | `dispatchThreadgroupsWithIndirectBuffer:indirectBufferOffset:` | 3×uint32 レイアウト互換 |
| `vkCmdPipelineBarrier` (buffer) | エンコーダ分割 or `memoryBarrierWithResources` | compute→compute はエンコーダを跨げば順序保証。 パスごとに 1 encoder が簡明 |
| fence + timeout | `MTLCommandBuffer waitUntilCompleted` + `addCompletedHandler` | ハング診断は completed handler + タイマ |
| present (fragment が SSBO 直読み) | `MTLRenderCommandEncoder` + fragment `device float4*` 読み | `CAMetalLayer` drawable へ描画。 `IOSSurfaceProvider` の layer をそのまま使う |
| `VK_EXT_metal_surface` (MoltenVK 用) | 不要 | provider は `CAMetalLayer` を既に保持しており流用可 |

## 4. シェーダ移植表 (GLSL → MSL)

対象 7 本: `sharc_hit / march / compact / update / resolve .comp`,
`sharc_present .vert/.frag`。 いずれも標準機能のみで、 **機械的に置換可能**。

| GLSL | MSL |
|---|---|
| `layout(std430, binding=N) buffer` | `device T* buf [[buffer(N)]]` (std430 と Metal のレイアウトは本ケースでは一致 — vec3 を使わず vec4/スカラのみで構成済み) |
| `layout(binding=0) uniform SharcParams` | `constant SharcParams& params [[buffer(0)]]` |
| `shared float x[...]` | `threadgroup float x[...]` |
| `barrier()` | `threadgroup_barrier(mem_flags::mem_threadgroup)` |
| `atomicCompSwap(a, cmp, val)` | `atomic_compare_exchange_weak_explicit((device atomic_uint*)&a, &cmp, val, memory_order_relaxed, memory_order_relaxed)` |
| `atomicAdd / atomicExchange` | `atomic_fetch_add_explicit / atomic_exchange_explicit` (relaxed) |
| `gl_GlobalInvocationID / gl_WorkGroupID / gl_LocalInvocationID` | `[[thread_position_in_grid]] / [[threadgroup_position_in_grid]] / [[thread_position_in_threadgroup]]` |
| `packHalf2x16 / unpackHalf2x16` | `as_type<uint>(half2(...)) / half2(as_type<half2>(v))` |
| `floatBitsToUint / uintBitsToFloat` | `as_type<uint> / as_type<float>` |
| `findMSB` 等 | 未使用 (確認済み) |
| RGB9E5 / oct16 / oct32 / ハッシュ | 手実装のためそのまま移植 |
| `gl_VertexIndex` (present.vert) | `[[vertex_id]]` |
| push constant (present) | `setBytes` + `constant PresentParams&` |

注意点:

- MSL の `atomic_uint` はバッファ型に混在できないため、 keys / stamps /
  counters は `device atomic_uint*` として別名バインドする (データは同じ
  `MTLBuffer`)。 非 atomic 読み (fast path) は同バッファを `device uint*`
  で重ねてバインドすれば良い (Metal はエイリアス可)。
- threadgroup メモリ使用量: update パスで 64×(16+16+4+4+4) + 64×39×4
  ≈ **13.5KB** — Apple GPU の 32KB 制限内。
- SIMD 幅 32 (Apple) vs 32 (NVIDIA warp)。 縮約は `barrier()` ベースで
  幅非依存に書いてあるため影響なし。

## 5. Apple GPU (A17 Pro) 特性と設計への影響

- **UMA**: PCIe が存在しないため、 デスクトップで支配的だった
  「host-visible 越しアクセス」問題が構造的に消える。 `storageModeShared`
  でも Private とほぼ同帯域 (書き込みパターン依存)。 まず全部 Shared で
  実装し、 プロファイル後に Private へ移す。
- **TBDR**: compute 主体の SHaRC には無関係。 present の 1 パスのみ。
- **性能見積**: A17 Pro ≈ 2.1〜2.6 TFLOPs (GTX 1070 の ~1/3)。
  §6 プリセットで補正する。
- **メモリ**: シーン (Bistro) 198MB + セル 20MB + レイ/シェード ~90MB
  ≈ 350MB — iPhone 15 Pro (8GB) で許容。 モバイル向けには §6 で削減。

## 6. モバイルプリセット

| 項目 | デスクトップ | モバイル (iPhone 15+) |
|---|---|---|
| 内部解像度 | 1280×720 | 960×540 (→ MetalFX で表示解像度へ) |
| `SHARC_LOBE_COUNT` | 4 (セル 80B) | 2 (セル 56B) |
| table_size | 262,144 | 131,072 |
| samples/cell (update) | 64 | 32 |
| max_ray_steps | 64 | 48 |
| セルストレージ | 20MB | 7MB |
| テクスチャ焼き込み | 起動時 CPU ベイク | **事前ベイクしてアセット化** (モバイル CPU での 130 PNG デコードを避ける) |

## 7. 実装計画 (macOS セッションへの引き継ぎ)

1. **M1**: Xcode ターゲット追加 (CMake `IOS` 分岐は既存 `:37-44`)。
   §4 の表に従い 7 シェーダを MSL へ機械変換、 `metal` ツールチェーンで
   コンパイル検証 (実機不要)。
2. **M2**: `SharcMetalExecutor` (ObjC++、 `SharcGpuExecutor` と同一公開 API)。
   バッファ生成 / 5+1 パスのエンコード / completed handler 診断。
3. **M3**: `MTKView` ベースの最小 iOS デモ shell。 `IOSSurfaceProvider` の
   `CAMetalLayer` を流用。 タッチで orbit カメラ。
4. **M4**: A17 実機プロファイル (Metal System Trace)。 §6 プリセットの
   実測調整。 サーマル挙動は `MobileLifecycleController` の thermal
   ダウングレードへ接続。

## 8. リスク

- MSL 移植の細部 (atomic エイリアス / half 丸め差) — M1 のコンパイル検証と
  白 furnace テスト (spec §5) で吸収する
- indirect dispatch の引数解釈差 (threadgroups 数 vs threads 数) —
  Metal は threadgroup 数で Vulkan と同義、 だが要実機確認
- BVH 走査の私有スタック (uint[48]) はレジスタ圧が高い — Apple GPU で
  occupancy が落ちる場合はスタック 32 段 + 浅い BVH (葉 16 tri) を検討
- 本リポは macOS CI を持たない — MSL のビルド検証ジョブ追加が必要

## 9. 関連

- `dx12-backend-design.md` — RHI 抽象 (本書はその Metal 分岐の先行実装)
- `portability.md` / `portability/mobile.md` — モバイル現状マトリクス
- `pictor-sharc-ext-design.md` (ワークスペース直下) — SHaRC 本体設計
- PR #103 — SHaRC 実装一式 (シェーダ / executor / デモ)
