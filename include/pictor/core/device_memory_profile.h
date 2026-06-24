#pragma once

#include <cstdint>
#include <string>
#include <vector>

// ============================================================
// 統合GPU (UMA) / ReBAR 判定とアップロード戦略の決定。
//
// discrete GPU は VRAM が host から見えないため staging buffer 経由で
// device-local へコピーする。一方:
//   - 統合GPU (Integrated, UMA): メモリは CPU/GPU 共有 → staging は無駄な
//     余計コピー。host-visible device-local へ直接書く (Direct) のが速い。
//   - discrete + ReBAR: VRAM 全体が host-visible device-local として見える →
//     大きい host-visible device-local heap があれば Direct を選べる。
//
// 判定ロジックは Vulkan 非依存の plain 記述 (DeviceMemoryDesc) を入力に取り、
// headless で単体テストできる。Vk 型→DeviceMemoryDesc の変換は VulkanContext
// 側のアダプタが担う (境界 API は OOP / consumer に Vk を漏らさない)。
// ============================================================

namespace pictor {

enum class GpuKind : uint8_t {
    Unknown = 0,
    Discrete,     // 専用 VRAM
    Integrated,   // UMA (CPU と共有)
    Virtual,
    Cpu,          // ソフトウェアラスタライザ等
};

const char* to_string(GpuKind kind);

/// CPU→GPU アップロード戦略。
enum class UploadPolicy : uint8_t {
    Staging = 0,  // host-visible staging → device-local へ copy
    Direct,       // host-visible device-local へ直接書き、staging を省略
};

const char* to_string(UploadPolicy policy);

// ---- Vulkan 非依存の入力記述 ----

struct MemoryHeapDesc {
    uint64_t size_bytes   = 0;
    bool     device_local = false;
};

struct MemoryTypeDesc {
    uint32_t heap_index   = 0;
    bool     device_local = false;
    bool     host_visible = false;
    bool     host_coherent = false;
};

struct DeviceMemoryDesc {
    GpuKind                     kind = GpuKind::Unknown;
    std::vector<MemoryHeapDesc> heaps;
    std::vector<MemoryTypeDesc> types;
};

// ---- 判定結果 ----

struct DeviceMemoryProfile {
    GpuKind      kind          = GpuKind::Unknown;
    bool         is_uma        = false;  // device-local かつ host-visible な type が存在
    bool         has_rebar     = false;  // 上記 type の heap が大きい (= VRAM 全体 mappable)
    UploadPolicy upload_policy = UploadPolicy::Staging;
    uint64_t     largest_device_local_heap = 0;
    uint64_t     direct_mappable_bytes = 0; // host-visible device-local heap の最大サイズ
    std::string  rationale;               // なぜその policy か (理由を必ず残す)
};

/// ReBAR とみなす host-visible device-local heap の下限。これ未満だと
/// 旧来の 256MB BAR ウィンドウ相当とみなし discrete は Staging を選ぶ。
constexpr uint64_t kReBarThresholdBytes = 256ull * 1024 * 1024;

/// メモリ記述からアップロード戦略を決定する (純粋関数・副作用なし)。
DeviceMemoryProfile analyze_device_memory(const DeviceMemoryDesc& desc);

} // namespace pictor
