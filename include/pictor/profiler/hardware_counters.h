#pragma once

#include <cstdint>
#include <memory>
#include <string>

// ============================================================
// C 層: Hardware performance counters (CPU cache / memory controller).
//
// 真の L2/L3 ヒット率・DRAM 帯域・IPC は OS/プロセス内から毎フレーム
// 安価には取れないため、ベンダ計測基盤 (Intel PCM) を opt-in で配線する。
// ビルドフラグ PICTOR_ENABLE_HW_COUNTERS=ON かつ PCM 連携時のみ実値。
// それ以外は NullHardwareCounterProvider が **理由付きで** available=false
// を返す (無言フォールバック禁止 — [[feedback_no_silent_fallback]])。
// ============================================================

namespace pictor {

/// 1 サンプル区間 ([begin_sample, end_sample]) のカウンタ差分。
/// 取得不能なカウンタは負値 (-1.0) / 0 で返す。
struct HwCounterSample {
    bool        available = false;
    std::string source;             // "Intel PCM" / "disabled"
    std::string unavailable_reason; // available==false のとき理由

    double   l2_hit_ratio = -1.0;   // 0..1
    double   l3_hit_ratio = -1.0;   // 0..1
    uint64_t l2_misses    = 0;
    uint64_t l3_misses    = 0;
    double   bytes_read_mc  = 0.0;  // メモリコントローラ read バイト
    double   bytes_write_mc = 0.0;  // メモリコントローラ write バイト
    double   instructions_retired = 0.0;
    double   ipc = -1.0;            // instructions per cycle
};

/// HW カウンタ provider (Open/Closed: backend 追加は派生のみ、consumer 無改変)。
///   - NullHardwareCounterProvider : 常に available=false + 理由
///   - PcmHardwareCounterProvider  : Intel PCM (PICTOR_ENABLE_HW_COUNTERS=ON)
class IHardwareCounterProvider {
public:
    virtual ~IHardwareCounterProvider() = default;

    /// 実カウンタが読めるか (host/build 依存)。
    virtual bool available() const = 0;

    /// backend 名 ("Intel PCM" / "disabled")。
    virtual const char* source() const = 0;

    /// 取得不能時の理由 (available()==true なら空)。
    virtual const char* unavailable_reason() const = 0;

    /// 計測区間の開始 (カウンタ状態をラッチ)。
    virtual void begin_sample() = 0;

    /// 計測区間の終了 — begin_sample からの差分を返す。
    virtual HwCounterSample end_sample() = 0;
};

/// Factory — PICTOR_ENABLE_HW_COUNTERS かつ host 対応時は PCM-backed を、
/// それ以外は理由付き null provider を返す (決して無言で偽値を返さない)。
std::unique_ptr<IHardwareCounterProvider> create_hardware_counter_provider();

} // namespace pictor
