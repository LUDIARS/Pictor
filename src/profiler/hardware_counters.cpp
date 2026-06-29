#include "pictor/profiler/hardware_counters.h"

// ============================================================
// C 層 HW カウンタの実装。
//
//   - NullHardwareCounterProvider : 常に利用不可 (理由付き)。HW 連携無効ビルドの既定。
//   - PcmHardwareCounterProvider  : Intel PCM (PICTOR_ENABLE_HW_COUNTERS=ON)。
//
// PCM はプロセス内から CPU の L2/L3 ミス・DRAM 帯域・IPC を読む。program()
// が失敗 (権限不足 / 他プロセスが MSR を専有 / 非対応 CPU) したら、その理由を
// そのまま乗せて available=false で返す (無言フォールバック禁止)。
// ============================================================

namespace pictor {

namespace {

/// HW カウンタが使えない場合の正直な provider。理由を必ず返す。
class NullHardwareCounterProvider final : public IHardwareCounterProvider {
public:
    explicit NullHardwareCounterProvider(std::string reason)
        : reason_(std::move(reason)) {}

    bool available() const override { return false; }
    const char* source() const override { return "disabled"; }
    const char* unavailable_reason() const override { return reason_.c_str(); }

    void begin_sample() override {}
    HwCounterSample end_sample() override {
        HwCounterSample s;
        s.available = false;
        s.source = "disabled";
        s.unavailable_reason = reason_;
        return s;
    }

private:
    std::string reason_;
};

} // namespace

#ifdef PICTOR_ENABLE_HW_COUNTERS

} // namespace pictor

#include <cpucounters.h>   // Intel PCM (PICTOR_PCM_DIR で include/link を指す)

namespace pictor {

namespace {

/// Intel PCM-backed provider。program() 成否を握って正直に報告する。
class PcmHardwareCounterProvider final : public IHardwareCounterProvider {
public:
    PcmHardwareCounterProvider() {
        pcm_ = pcm::PCM::getInstance();
        const pcm::PCM::ErrorCode status = pcm_->program();
        switch (status) {
            case pcm::PCM::Success:
                ok_ = true;
                break;
            case pcm::PCM::MSRAccessDenied:
                reason_ = "PCM: MSR access denied (要 administrator/権限、または MSR ドライバ未導入)";
                break;
            case pcm::PCM::PMUBusy:
                reason_ = "PCM: PMU busy (他プロセスが性能カウンタを専有中)";
                break;
            default:
                reason_ = "PCM: program() failed (非対応 CPU か初期化失敗)";
                break;
        }
    }

    bool available() const override { return ok_; }
    const char* source() const override { return "Intel PCM"; }
    const char* unavailable_reason() const override { return reason_.c_str(); }

    void begin_sample() override {
        if (!ok_) return;
        before_ = pcm::getSystemCounterState();
    }

    HwCounterSample end_sample() override {
        HwCounterSample s;
        s.source = "Intel PCM";
        if (!ok_) {
            s.available = false;
            s.unavailable_reason = reason_;
            return s;
        }
        const pcm::SystemCounterState after = pcm::getSystemCounterState();
        s.available = true;
        s.l2_hit_ratio = pcm::getL2CacheHitRatio(before_, after);
        s.l3_hit_ratio = pcm::getL3CacheHitRatio(before_, after);
        s.l2_misses    = pcm::getL2CacheMisses(before_, after);
        s.l3_misses    = pcm::getL3CacheMisses(before_, after);
        s.bytes_read_mc  = pcm::getBytesReadFromMC(before_, after);
        s.bytes_write_mc = pcm::getBytesWrittenToMC(before_, after);
        s.instructions_retired = static_cast<double>(pcm::getInstructionsRetired(before_, after));
        s.ipc = pcm::getIPC(before_, after);
        return s;
    }

private:
    pcm::PCM*                 pcm_ = nullptr;
    pcm::SystemCounterState   before_{};
    bool                      ok_  = false;
    std::string               reason_;
};

} // namespace

std::unique_ptr<IHardwareCounterProvider> create_hardware_counter_provider() {
    auto provider = std::make_unique<PcmHardwareCounterProvider>();
    if (provider->available()) {
        return provider;
    }
    // PCM 初期化に失敗 — null へ落とすが理由は PCM の文言を引き継ぐ。
    return std::make_unique<NullHardwareCounterProvider>(provider->unavailable_reason());
}

#else  // PICTOR_ENABLE_HW_COUNTERS

std::unique_ptr<IHardwareCounterProvider> create_hardware_counter_provider() {
    return std::make_unique<NullHardwareCounterProvider>(
        "PICTOR_ENABLE_HW_COUNTERS=OFF (HW カウンタ連携は未ビルド)");
}

#endif // PICTOR_ENABLE_HW_COUNTERS

} // namespace pictor
