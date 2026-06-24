/// UMA / ReBAR アップロード戦略判定の単体テスト (headless, Vk 非依存)。
/// spec/subsystem/uma_memory.md を裏取りする。

#include "pictor/core/device_memory_profile.h"
#include "test_common.h"

using namespace pictor;
using namespace pictor_test;

namespace {

// discrete GPU: device-local VRAM heap + 別の host-visible system heap。
// host-visible device-local 無し → staging。
DeviceMemoryDesc make_discrete_no_rebar() {
    DeviceMemoryDesc d;
    d.kind = GpuKind::Discrete;
    d.heaps = {
        { 8ull * 1024 * 1024 * 1024, true  }, // 8GB VRAM (device-local)
        { 16ull * 1024 * 1024 * 1024, false }, // 16GB system (host)
    };
    d.types = {
        { 0, /*dl*/true,  /*hv*/false, false }, // device-local only
        { 1, /*dl*/false, /*hv*/true,  true  }, // host-visible only
    };
    return d;
}

// discrete + ReBAR: 大きい host-visible device-local heap。
DeviceMemoryDesc make_discrete_rebar() {
    DeviceMemoryDesc d;
    d.kind = GpuKind::Discrete;
    d.heaps = {
        { 8ull * 1024 * 1024 * 1024, true }, // 8GB device-local, host から見える
        { 16ull * 1024 * 1024 * 1024, false },
    };
    d.types = {
        { 0, true,  true,  true  }, // device-local + host-visible (ReBAR)
        { 1, false, true,  true  },
    };
    return d;
}

// integrated (UMA): 共有メモリ、device-local かつ host-visible。
DeviceMemoryDesc make_integrated() {
    DeviceMemoryDesc d;
    d.kind = GpuKind::Integrated;
    d.heaps = { { 16ull * 1024 * 1024 * 1024, true } }; // 共有 16GB
    d.types = {
        { 0, true, false, false }, // device-local
        { 0, true, true,  true  }, // device-local + host-visible (UMA)
    };
    return d;
}

} // namespace

int main() {
    // discrete (ReBAR 無し) → Staging。
    {
        const DeviceMemoryProfile p = analyze_device_memory(make_discrete_no_rebar());
        PT_ASSERT(p.kind == GpuKind::Discrete, "discrete kind");
        PT_ASSERT(!p.is_uma, "discrete no host-visible device-local → not uma");
        PT_ASSERT(!p.has_rebar, "no rebar");
        PT_ASSERT(p.upload_policy == UploadPolicy::Staging, "discrete no-rebar → Staging");
        PT_ASSERT(!p.rationale.empty(), "rationale recorded");
    }
    // discrete + ReBAR → Direct。
    {
        const DeviceMemoryProfile p = analyze_device_memory(make_discrete_rebar());
        PT_ASSERT(p.is_uma, "host-visible device-local present");
        PT_ASSERT(p.has_rebar, "large host-visible device-local heap → rebar");
        PT_ASSERT(p.upload_policy == UploadPolicy::Direct, "discrete+rebar → Direct");
        PT_ASSERT_OP(p.direct_mappable_bytes, >=, kReBarThresholdBytes, "mappable >= threshold");
    }
    // integrated (UMA) → Direct。
    {
        const DeviceMemoryProfile p = analyze_device_memory(make_integrated());
        PT_ASSERT(p.kind == GpuKind::Integrated, "integrated kind");
        PT_ASSERT(p.is_uma, "integrated is uma");
        PT_ASSERT(p.upload_policy == UploadPolicy::Direct, "integrated → Direct (staging 省略)");
        PT_ASSERT(!p.rationale.empty(), "rationale recorded");
    }
    // 空 desc (Unknown) → 安全側 Staging。
    {
        const DeviceMemoryProfile p = analyze_device_memory(DeviceMemoryDesc{});
        PT_ASSERT(p.upload_policy == UploadPolicy::Staging, "unknown → Staging (安全側)");
    }

    return report("unit_device_memory_profile_test");
}
