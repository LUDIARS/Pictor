# Portability — Mobile (Android / iOS)

モバイル対応の現状・実装課題・設計。 総括は [../portability.md](../portability.md)、
ビルド手順の詳細は `docs/android-build.md` (Issue #47 の段階計画) を参照。

---

## 1. 現状 (実装済みの範囲)

### ビルド分岐 — 実装済み・未検証

- `CMakeLists.txt:29-45`: `ANDROID` / `IOS` で早期分岐し `PICTOR_MOBILE` を設定。
  Android は NDK 同梱 Vulkan を直指定、 iOS は MoltenVK を host xcodeproj が
  リンクする前提 (`Vulkan_LIBRARIES` 未設定のまま `PICTOR_HAS_VULKAN` を立てる)。
- モバイルでは GLFW FetchContent / demo / tests / tools をすべて除外
  (`:54-73`, `:328-331`, `:640-643`, `:656`)。
- Android リンク: `vulkan android log` + `VK_USE_PLATFORM_ANDROID_KHR`
  (`:234-237`)。 iOS: `VK_USE_PLATFORM_METAL_EXT` (`:238-246`)。
- **注意**: NDK / macOS 環境が無いためクロスビルドは一度も検証されていない
  (docs/android-build.md:11-12, 301-305)。 デスクトップ回帰のみ確認済み。

### Surface — 実装済み

- `AndroidSurfaceProvider` (`src/surface/android_surface_provider.cpp`):
  `ANativeWindow*` 保持、 `VK_KHR_android_surface` 要求、 surfaceChanged 用
  `update_window()`。
- `IOSSurfaceProvider` (`src/surface/ios_surface_provider.cpp`):
  `CAMetalLayer` (void*) 保持、 `VK_EXT_metal_surface` 要求、 `update_layer()`。
- `VulkanContext::create_surface()` は Android / iOS 両 case を実装
  (`vulkan_context.cpp:355-384`)。 iOS case 欠落 (review/2026-06-11 Q-2 —
  「iOS は必ず初期化失敗」) は **修正済み**。
- swapchain 再生成は `VK_ERROR_OUT_OF_DATE_KHR` / `SUBOPTIMAL` で実装済み
  (`vulkan_context.cpp:124-129, 168-169`)。

### ライフサイクル — 実装済み・実機配線なし・テストなし

- `MobileLifecycleController` (D-1 切り出し):
  pause / resume / suspend / surface lost-regain / low-memory / thermal の
  ステートマシン + thermal 自動ダウングレード (opt-in、
  `MobileAutoDowngradePolicy`)。 resume 時の frame allocator フラッシュ、
  ACTIVE 以外での per-frame 作業抑制 (`frame_work_suppressed()`) を提供。
- host が提供する Hooks: `current_frame` / `flush_frame_allocator` /
  `active_profile` / `switch_profile` (`mobile_lifecycle_controller.h:19-24`)。
  swapchain 再生成は host 責務 (`mobile_lifecycle.h:20-21`)。
- 演習は `demo/mobile/main.cpp` (シミュレーション) のみ。 **ユニットテスト無し**
  (かつモバイルビルドはテスト除外)。

### メモリ / SIMD — 部分実装

- `DeviceMemoryProfile` (`device_memory_profile.cpp:24-73`): UMA / ReBAR 判定 →
  `UploadPolicy::Direct/Staging`。 実装 + ユニットテスト済み。 ただし
  **実際のアップロードコピー (staging スキップ) は未実装**
  (spec/subsystem/uma_memory.md:41-46 が明記)。
- AVX2 は x86 かつ非モバイルのみ (`CMakeLists.txt:312-314`)。 ARM は
  `#ifdef __AVX2__` が落ちてスカラ。 **NEON 経路は存在しない**
  (方針: スカラ→計測→NEON、 docs/android-build.md:341)。
- cache line は arch 対応済み: Apple arm64 = 128B (`types.h:37-43`)。
- large pages (`PICTOR_LARGE_PAGES`) は Win/POSIX 実装のみ。 モバイルでは
  マクロを立てない運用 (既定 OFF)。

## 2. 実装課題 (ギャップ一覧)

| # | 課題 | 事実 (根拠) | 影響 |
|---|---|---|---|
| M-P1 | **クロスビルド未検証** | NDK ビルドが一度も走っていない (android-build.md:301) | 分岐コードが無検証のまま腐る |
| M-P2 | **JNI / Gradle (Phase 2-3) 未実装** | `android/` ディレクトリ・GameActivity 配線なし | 実機で動かす入口が無い |
| M-P3 | **GPU プール予算がデスクトップ固定** | 既定 ~528MB (`gpu_memory_allocator.h:23-27`)、 profile から注入不可 | ローエンド機で確保失敗 (今回の autofix で失敗検出は入ったが、 予算調整手段が無い) |
| M-P4 | **UMA Direct アップロード実コピー未実装** | uploader はスタブ (uma_memory.md:41-46) | モバイル (全機 UMA) で staging 二度書きの帯域損 |
| M-P5 | **lifecycle のテスト不在** | tests/ に該当なし + モバイルビルドはテスト除外 | thermal 自動ダウングレード等の回帰が守られない (実際 2026-07-09 に空文字 sentinel バグを修正した) |
| M-P6 | **c_api にモバイル入口なし** | c_api.cpp に lifecycle / surface 系シンボル 0 件 | JNI から C API 経由で叩けない (C++ API 直結が必要になる) |
| M-P7 | **HW カウンタは Intel PCM (x86) のみ** | `hardware_counters.cpp:43-47` | モバイルでは Null provider に degrade (正直だが計測不能) |
| M-P8 | **`MemoryConfig::use_large_pages` が虚偽 API** | serialize されるが runtime 未参照 (review L-8) | プロファイルで設定しても効かない |
| M-P9 | NEON 経路なし | ARM はスカラのみ | 性能 (方針どおり計測後で可) |
| M-P10 | iOS は scaffolding のみ | android-build.md §11 (Issue #47 スコープ外) | iOS 実機は未着手と明示されている |

## 3. 設計 (対応方針)

### Phase A — 「壊れない」 を CI で保証する (M-P1)

GitHub Actions に NDK ツールチェーンの **コンパイルのみ** ジョブを追加する
(arm64-v8a、 `-DANDROID_PLATFORM=android-29`)。 リンク・実行はしない。
これだけで `PICTOR_MOBILE` 分岐 / provider / `#ifdef` の腐敗が止まる。
テスト除外はそのまま (headless テストはデスクトップ CI が担保)。

### Phase B — 実機入口 (M-P2, M-P6)

docs/android-build.md Phase 2-3 に従う:
1. エミュレータ x86_64 で `libpictor.a` + 最小 JNI shim (`android/` 新設)。
2. JNI からは **C API を拡張して** 叩く: `pictor_on_pause()` /
   `pictor_on_resume()` / `pictor_on_surface_changed(ANativeWindow*)` /
   `pictor_on_thermal(int)` を c_api に追加し、 内部で
   `MobileLifecycleController` へ転送する。 C++ API 直結 (M-P6 の現状) は
   ODR / 例外境界の観点で JNI に不向き。
3. GameActivity + Gradle は最後 (成果物: サンプル APK)。

### Phase C — メモリ予算 (M-P3, M-P8)

- `GpuMemoryAllocator::Config` を `PipelineProfileDef` から注入可能にする
  (serializer は既にプール系フィールドを持つ `MemoryConfig` を読んでいる —
  そこに pool サイズを足し、 `MemorySubsystem` 構築時へ配線)。
- 同時に `use_large_pages` を「実装するか消すか」 決める。 モバイルでは
  効かないフラグなので、 **runtime 参照を実装せず serializer / struct から
  削除する** 方を推奨 (虚偽 API の解消、 大ページはコンパイル時マクロに一本化)。
- MobileLow / MobileHigh プリセット (thermal 自動ダウングレードの既定名) に
  実予算を定義: 例 MobileLow = mesh 64MB / ssbo 32MB / instance 16MB×2 /
  staging 16MB×2。 数値は Phase B の実機計測で確定する。

### Phase D — UMA 実装 (M-P4)

`should_stage() == false` の経路で staging を経由しない
`vkMapMemory` 直書きを `vertex_data_uploader` / instance データに実装。
decision seam (`DeviceMemoryProfile`) とテストは既にあるので、 コピー経路の
差し替えだけ。 実装までは現状どおり 「常に staging (安全側)」 を維持し、
未実装を偽装しない。

### Phase E — テスト (M-P5)

`MobileLifecycleController` は Hooks が全部 `std::function` なので
headless で完結にテスト可能。 追加すべきケース:
- thermal SERIOUS→FAIR で downgrade→restore が 1 回ずつ発火する
- `active_profile()` が空文字を返す host でも再発火しない (2026-07-09 修正の回帰)
- SURFACE_LOST 中の pause/resume が状態を壊さない
- resume 時に `flush_frame_allocator` が呼ばれる

### 非対応と明示するもの

- iOS 実機 (MoltenVK 統合・xcframework) は Issue #47 のスコープ外を維持。
  provider + surface case が既にあるため、 着手時は Phase B の iOS 版
  (ObjC++ shim) から。
- NEON は計測でボトルネックが出るまで書かない (スカラ優先の決定を維持)。
