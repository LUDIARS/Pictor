#include "pictor/core/device_memory_profile.h"

namespace pictor {

const char* to_string(GpuKind kind) {
    switch (kind) {
        case GpuKind::Unknown:    return "Unknown";
        case GpuKind::Discrete:   return "Discrete";
        case GpuKind::Integrated: return "Integrated";
        case GpuKind::Virtual:    return "Virtual";
        case GpuKind::Cpu:        return "Cpu";
    }
    return "Unknown";
}

const char* to_string(UploadPolicy policy) {
    switch (policy) {
        case UploadPolicy::Staging: return "Staging";
        case UploadPolicy::Direct:  return "Direct";
    }
    return "Staging";
}

DeviceMemoryProfile analyze_device_memory(const DeviceMemoryDesc& desc) {
    DeviceMemoryProfile p;
    p.kind = desc.kind;

    // device-local heap の最大サイズ。
    for (const auto& h : desc.heaps) {
        if (h.device_local && h.size_bytes > p.largest_device_local_heap) {
            p.largest_device_local_heap = h.size_bytes;
        }
    }

    // host-visible かつ device-local な memory type が UMA / ReBAR の鍵。
    for (const auto& t : desc.types) {
        if (t.device_local && t.host_visible) {
            p.is_uma = true;
            const uint64_t heap_size =
                (t.heap_index < desc.heaps.size()) ? desc.heaps[t.heap_index].size_bytes : 0;
            if (heap_size > p.direct_mappable_bytes) p.direct_mappable_bytes = heap_size;
        }
    }
    p.has_rebar = p.is_uma && (p.direct_mappable_bytes >= kReBarThresholdBytes);

    // 戦略決定 (理由を必ず残す — 無言フォールバック禁止)。
    if (desc.kind == GpuKind::Integrated) {
        if (p.is_uma) {
            p.upload_policy = UploadPolicy::Direct;
            p.rationale = "統合GPU(UMA): host-visible device-local 有 → staging 省略し直接書き";
        } else {
            // 統合GPU なのに host-visible device-local が無いのは異例。安全側で staging。
            p.upload_policy = UploadPolicy::Staging;
            p.rationale = "統合GPU だが host-visible device-local が無い → 安全側で staging";
        }
    } else if (desc.kind == GpuKind::Discrete) {
        if (p.has_rebar) {
            p.upload_policy = UploadPolicy::Direct;
            p.rationale = "discrete + ReBAR (host-visible device-local heap が大) → 直接書き可";
        } else {
            p.upload_policy = UploadPolicy::Staging;
            p.rationale = p.is_uma
                ? "discrete: host-visible device-local heap が小 (256MB BAR 窓相当) → staging"
                : "discrete: VRAM は host から不可視 → staging 経由でコピー";
        }
    } else {
        // Cpu / Virtual / Unknown: 安全側で staging。
        p.upload_policy = UploadPolicy::Staging;
        p.rationale = std::string("device kind=") + to_string(desc.kind) + " → 安全側で staging";
    }

    return p;
}

} // namespace pictor
