// Headless checks for the typed mobile surface / device recovery contract
// (spec/feature/portability/mobile-surface-recovery.md). Each test names the
// acceptance contract id (C-n) of spec/tasks/2026-10-01-mobile-surface-recovery-contract.md.

#include "pictor/surface/context_init_result.h"
#include "pictor/surface/frame_gate.h"
#include "pictor/surface/frame_result.h"
#include "pictor/surface/vulkan_capability_check.h"
#include "pictor/surface/vulkan_context.h"
#include "test_common.h"

#include <cstring>
#include <string>
#include <vector>

using namespace pictor;
using namespace pictor_test;

namespace {

/// Host-owned provider double. Counts native queries so the test can prove
/// that no instance creation was attempted while the window is absent.
class FakeProvider final : public ISurfaceProvider {
public:
    NativeWindowHandle get_native_handle() const override {
        ++handle_queries;
        NativeWindowHandle h;
        h.type = NativeWindowHandle::Type::None;
        return h;
    }
    SwapchainConfig get_swapchain_config() const override { return {}; }
    uint32_t get_required_instance_extensions(const char**, uint32_t) const override {
        ++extension_queries;
        return 0;
    }

    mutable int handle_queries = 0;
    mutable int extension_queries = 0;
};

// C-1: the gate precedence and that only Ready lets a native call through.
void test_gate_frame_precedence() {
    FrameGateInput in;
    PT_ASSERT(gate_frame(in).status == FrameStatus::NotInitialized,
              "C-1 uninitialized context is NotInitialized");

    in.initialized = true;
    in.latched = FrameStatus::Ready;
    in.native_surface_available = true;
    PT_ASSERT(gate_frame(in).status == FrameStatus::Ready, "C-1 healthy context proceeds");

    in.latched = FrameStatus::RecreateSwapchain;
    PT_ASSERT(gate_frame(in).status == FrameStatus::Ready,
              "C-1 resize recovery is not sticky");

    const FrameStatus sticky[] = {FrameStatus::SurfaceLost, FrameStatus::DeviceLost,
                                  FrameStatus::Error};
    for (FrameStatus s : sticky) {
        in.latched = s;
        in.presentation_suspended = true;
        PT_ASSERT(gate_frame(in).status == s, "C-1 loss / error stays latched over suspension");
        PT_ASSERT(frame_status_requires_reinitialize(s), "C-1 loss requires reinitialize");
    }

    in.latched = FrameStatus::Suspended;
    in.presentation_suspended = true;
    in.native_surface_available = false;
    PT_ASSERT(gate_frame(in).status == FrameStatus::Suspended,
              "C-1 suspension wins over a released native window");

    in.presentation_suspended = false;
    PT_ASSERT(gate_frame(in).status == FrameStatus::SurfaceLost,
              "C-1 unsuspended context without a native window is SurfaceLost");

    in.native_surface_available = true;
    PT_ASSERT(gate_frame(in).status == FrameStatus::Ready,
              "C-1 resuming after suspension proceeds");
    PT_ASSERT(!gate_frame(in).has_image(), "C-1 the gate never hands out an image");
    PT_ASSERT(!frame_status_requires_reinitialize(FrameStatus::Suspended),
              "C-1 suspension is not a loss");
    PT_ASSERT(!frame_status_requires_reinitialize(FrameStatus::RecreateSwapchain),
              "C-1 resize is not a loss");
}

// C-2: no native window → nothing native is created, typed reason recorded.
void test_initialize_without_native_window() {
#ifdef PICTOR_HAS_VULKAN
    FakeProvider provider;
    VulkanContext context;
    PT_ASSERT(context.init_result().status == ContextInitStatus::NotInitialized,
              "C-2 fresh context has no init outcome");
    PT_ASSERT(!context.initialize(&provider), "C-2 initialize refuses a missing window");
    PT_ASSERT(context.init_result().status == ContextInitStatus::SurfaceUnavailable,
              "C-2 missing window reports SurfaceUnavailable");
    PT_ASSERT(!context.init_result().detail.empty(), "C-2 reason is named");
    PT_ASSERT(provider.extension_queries == 0,
              "C-2 instance creation was not attempted");
    PT_ASSERT(!context.is_initialized(), "C-2 context stays uninitialized");
    PT_ASSERT(context.acquire_frame().status == FrameStatus::NotInitialized,
              "C-2 acquire after refused init does not touch Vulkan");

    VulkanContext no_provider;
    PT_ASSERT(!no_provider.initialize(nullptr), "C-2 null provider is refused");
    PT_ASSERT(no_provider.init_result().status == ContextInitStatus::Failed,
              "C-2 null provider is a failure, not a missing surface");
#endif
}

// C-3: missing extensions are named, never folded into resize / skip.
void test_first_missing_extension() {
    const std::vector<std::string> available = {"VK_KHR_surface", "VK_KHR_swapchain"};
    PT_ASSERT(first_missing_extension({"VK_KHR_surface"}, available) == nullptr,
              "C-3 available extension is not missing");
    const char* missing = first_missing_extension(
        {"VK_KHR_surface", "VK_KHR_android_surface", "VK_EXT_metal_surface"}, available);
    PT_ASSERT(missing && std::strcmp(missing, "VK_KHR_android_surface") == 0,
              "C-3 the first missing extension is reported by name");
    PT_ASSERT(first_missing_extension({}, available) == nullptr,
              "C-3 empty requirement is satisfied");
    PT_ASSERT(first_missing_extension({"VK_KHR_surface"}, {}) != nullptr,
              "C-3 empty availability misses everything");

    ContextInitResult missing_ext{ContextInitStatus::MissingInstanceExtension, missing};
    PT_ASSERT(!missing_ext.ok(), "C-3 missing extension is not a successful init");
    PT_ASSERT(missing_ext.status != ContextInitStatus::SurfaceUnavailable,
              "C-3 missing extension is distinct from an absent surface");
}

// C-4: portability subset features are only restrictive on subset devices.
void test_portability_feature_check() {
    PortabilityFeatures required;
    required.triangle_fans = true;
    required.events = true;

    PortabilityCapabilities conformant;
    PT_ASSERT(first_missing_portability_feature(conformant, required) == nullptr,
              "C-4 conformant device supports every portability feature");

    PortabilityCapabilities subset;
    subset.subset_device = true;
    subset.features.events = true;
    const char* missing = first_missing_portability_feature(subset, required);
    PT_ASSERT(missing && std::strcmp(missing, "triangleFans") == 0,
              "C-4 unsupported subset feature is reported by name");

    subset.features.triangle_fans = true;
    PT_ASSERT(first_missing_portability_feature(subset, required) == nullptr,
              "C-4 subset device with every required feature passes");
    PT_ASSERT(first_missing_portability_feature(subset, PortabilityFeatures{}) == nullptr,
              "C-4 no requirement always passes");
    PT_ASSERT(std::strcmp(kPortabilitySubsetExtensionName, "VK_KHR_portability_subset") == 0,
              "C-4 portability subset extension name matches the registry");

    VulkanContextConfig cfg;
    PT_ASSERT(first_missing_portability_feature(subset, cfg.required_portability_features) == nullptr,
              "C-4 default config requires no portability feature");
}

// C-5: suspension is host state; it never hands out an image.
void test_presentation_suspension() {
    VulkanContext context;
    PT_ASSERT(!context.presentation_suspended(), "C-5 presentation starts unsuspended");
    context.set_presentation_suspended(true);
    PT_ASSERT(context.presentation_suspended(), "C-5 suspension is recorded");
    const FrameResult acquired = context.acquire_frame();
    PT_ASSERT(!acquired.has_image(), "C-5 suspended context hands out no image");
    PT_ASSERT(context.acquire_next_image() == UINT32_MAX,
              "C-5 legacy acquire also yields no image");
    PT_ASSERT(!context.present(0), "C-5 legacy present does not queue");
    context.set_presentation_suspended(false);
    PT_ASSERT(!context.presentation_suspended(), "C-5 suspension is cleared");

    FrameGateInput in;
    in.initialized = true;
    in.latched = FrameStatus::Ready;
    in.native_surface_available = true;
    in.presentation_suspended = true;
    const FrameResult gated = gate_frame(in);
    PT_ASSERT(gated.status == FrameStatus::Suspended && !gated.has_image(),
              "C-5 initialized but suspended context reports Suspended without an image");
}

// C-6: recreation is reported separately from the image grant.
void test_swapchain_recreated_flag() {
    FrameResult fresh;
    PT_ASSERT(!fresh.swapchain_recreated, "C-6 results default to no recreation");
    FrameResult legacy{FrameStatus::Ready, 2, true};
    PT_ASSERT(!legacy.swapchain_recreated && legacy.has_image(),
              "C-6 positional initialization keeps its meaning");
    FrameResult rebuilt{FrameStatus::RecreateSwapchain, UINT32_MAX, false, true};
    PT_ASSERT(rebuilt.swapchain_recreated && !rebuilt.has_image(),
              "C-6 a rebuilt swapchain still grants no image this frame");

    VulkanContext context;
    PT_ASSERT(!context.last_frame_result().swapchain_recreated,
              "C-6 uninitialized context never reports recreation");
    PT_ASSERT(!context.recreate_swapchain(), "C-6 uninitialized recreate does nothing");
}

} // namespace

int main() {
    test_gate_frame_precedence();
    test_initialize_without_native_window();
    test_first_missing_extension();
    test_portability_feature_check();
    test_presentation_suspension();
    test_swapchain_recreated_flag();
    return report("unit_surface_recovery_contract_test");
}
