# Cache-Line Portability — マルチアーキ DoD 整列

> 対象: `include/pictor/core/types.h` (整列マクロ) /
> `core/cache_info.h/.cpp` (実機検出) / `profiler/perf_introspection.h`
> (ビルド vs 実機の可視化)。

## 1. 目的

DoD のキャッシュ整列を **x86-64 / ARM (Android) / Apple Silicon (iOS) /
POWER** で正しく機能させる。旧実装は 1 マクロ `PICTOR_CACHE_LINE_SIZE` が
「偽共有回避」と「データ充填」の 2 concern を兼ね、128B ライン機で
`float4x4` が膨れて `static_assert(sizeof==64)` が落ちるバグがあった。

## 2. アーキ別キャッシュライン (事実)

| アーキ | L1/L2 ライン | 既定 |
|---|---|---|
| x86-64 (PC: Intel/AMD) | 64B | 64 |
| ARM Cortex-A / Kryo (Android 主流 SoC) | 64B | 64 |
| **Apple Silicon (M 系) / 近年 A 系 (iOS)** | **128B** | **128** |
| POWER (ppc64) / 一部 ARM サーバ | 128B | 128 (Neoverse 等は `-D` 上書き) |

→ **iOS が 128B 側の外れ値**。Android は 64B で x86 と揃う
(SoC 差はライン幅にはほぼ出ない)。

## 3. concern 分離 (設計判断)

| マクロ | 用途 | 値 |
|---|---|---|
| `PICTOR_CACHE_LINE_SIZE` | 偽共有回避 + perf モニタの跨ぎ計算に使う「真のライン幅」 | アーキ別 (64/128)、`-D` 上書き可 |
| `PICTOR_FALSE_SHARING_ALIGN` | スレッド別データを 1 ライン専有 | `alignas(ライン幅)` |
| `PICTOR_SIMD_ALIGN` | 充填構造体 (`float4x4` 等) の SIMD 整列 | **固定 `alignas(16)`** |

`float4x4` は `PICTOR_SIMD_ALIGN` を使い、ライン幅を 128 にしても **64B を維持**
(transform ストリーム密度を保つ)。配列要素は PoolAllocator の 64B 起点 +
64B ストライドで自然にライン整列する。

## 4. ランタイム検出 → 可視化

`alignas` はコンパイル時固定で実行時に変えられない。そこで実機ライン幅を
`query_cache_line_info()` で取得し、`MemoryLayoutReport` に
`runtimeCacheLineSize` / `cacheLineMatches` / `cpuArch` を載せて perf モニタへ。
**「64B 想定バイナリを Apple 128B 機で実行 → 跨ぎ倍増」** のような誤ビルドを
視認できる (整列自体は直せないが検出はできる)。

検出経路:
- Windows: `GetLogicalProcessorInformation` の L1 データ/統合キャッシュ LineSize
- Apple: `sysctlbyname("hw.cachelinesize")`
- Linux/Android: `sysconf(_SC_LEVEL1_DCACHE_LINESIZE)` → 不可なら sysfs
  `coherency_line_size`

検出不能でも throw せず `runtime=0 / matches=true` (不明扱い、誤警告なし)。

## 5. テスト (`tests/unit_cache_info_test.cpp`)

- `sizeof(float4x4)==64` / `alignof(float4x4)==16` を全アーキで維持。
- ビルド時ライン幅が 2 のべき [32,256]。
- ランタイム検出値が妥当 (2 のべき) か 0 (不明)。`matches` が正しく反映。
- x86-64 ビルドは 64B に解決。
