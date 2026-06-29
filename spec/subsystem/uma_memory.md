# UMA / ReBAR メモリ最適化 — 統合GPU 向けアップロード戦略

> 対象: `core/device_memory_profile.h/.cpp` (判定) /
> `surface/vulkan_context.{h,cpp}` (Vk アダプタ) /
> `gpu/gpu_buffer_manager.h` (判定 seam)。

## 1. 目的

discrete GPU は VRAM が CPU から不可視なので staging buffer 経由で device-local
へコピーする。一方:

- **統合GPU (Integrated, UMA)**: メモリは CPU/GPU 共有。staging は**無駄な余計
  コピー** → host-visible device-local へ直接書く (Direct) のが速い。
- **discrete + ReBAR**: VRAM 全体が host-visible device-local に見える → 大きい
  host-visible device-local heap があれば Direct を選べる。

これを検出し、アップロード戦略 (`Staging` / `Direct`) を決める。

## 2. 設計 (concern 分離・テスト可能性)

判定ロジックは **Vulkan 非依存の純粋関数** `analyze_device_memory(DeviceMemoryDesc)`
として切り出す。Vk 型 (`VkPhysicalDeviceMemoryProperties`) → `DeviceMemoryDesc`
変換は `VulkanContext::describe_device_memory()` (Vk アダプタ) が担う。これにより
判定を **headless で単体テスト**できる (合成入力で discrete/integrated/ReBAR を再現)。

判定:
- `is_uma` = device-local かつ host-visible な memory type が存在。
- `has_rebar` = `is_uma` かつ その heap が `kReBarThresholdBytes`(256MB) 以上。
- `upload_policy`:
  - Integrated + UMA → **Direct**
  - Discrete + ReBAR → **Direct**
  - それ以外 (discrete no-rebar / Cpu / Unknown) → **Staging** (安全側)
- `rationale` に必ず理由を残す (無言フォールバック禁止)。

## 3. 配線 (現状の正直な範囲)

`GPUBufferManager::set_memory_profile()` / `should_stage()` を判定 seam として追加。
host は起動時に `buffer_manager.set_memory_profile(vk.device_memory_profile())` を
呼ぶ。production の upload 経路はこの `should_stage()` を見て staging コピーを省く。

> **重要 (正直な現状)**: `vertex_data_uploader` / `texture_registry` の実 upload は
> 現在**スタブ**(「In production: memcpy to staging, record copy command」コメントの
> まま、実 `vkCmdCopyBuffer` 未実装)。よって本 PR は **判定とその供給口までを実装**し、
> 実コピー実装時に `should_stage()==false` で Direct 経路を取れるよう配線する。
> 「staging を実際に省いた」と装わない — 実コピー経路の実装は別タスク。

## 4. テスト (`tests/unit_device_memory_profile_test.cpp`)

- discrete (host-visible device-local 無し) → Staging
- discrete + 大 host-visible device-local heap → Direct (ReBAR)
- integrated (UMA) → Direct
- 空/Unknown → Staging (安全側)
