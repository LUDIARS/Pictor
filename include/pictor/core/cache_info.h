#pragma once

#include "pictor/core/types.h"
#include <cstddef>
#include <string>

// ============================================================
// 実機の CPU キャッシュライン幅を検出する (multi-arch)。
//
// alignas はコンパイル時に固定されるため、ビルド時に選んだ
// PICTOR_CACHE_LINE_SIZE と実機のライン幅がズレても整列自体は変えられない。
// ここでは実機値を取得し「ビルド想定 vs 実機」を perf モニタで可視化して
// 誤ビルド (例: 64B 想定バイナリを Apple 128B 機で実行) を検出可能にする。
//
// 検出経路:
//   Windows : GetLogicalProcessorInformation の L1 データ/統合キャッシュ LineSize
//   Apple   : sysctlbyname("hw.cachelinesize")
//   Linux/Android : sysconf(_SC_LEVEL1_DCACHE_LINESIZE)
//                   → 取れなければ /sys の coherency_line_size を読む
// ============================================================

namespace pictor {

struct CacheLineInfo {
    size_t      compiled_line_size = PICTOR_CACHE_LINE_SIZE; // ビルド時の想定ライン幅
    size_t      runtime_line_size  = 0;     // 実機 L1D ライン幅 (0 = 検出不能)
    bool        matches            = true;  // runtime==0 (不明) または runtime==compiled
    std::string arch;                        // "x86-64" / "arm64" / "arm64-apple" / ...
    std::string source;                      // 検出経路名 ("win32-logical-processor" 等)
};

/// 実機のキャッシュライン情報を取得する。検出不能でも throw せず
/// runtime_line_size=0 / matches=true (不明扱い) を返す。
CacheLineInfo query_cache_line_info();

} // namespace pictor
