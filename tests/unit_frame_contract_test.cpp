#include "pictor/core/mobile_lifecycle_controller.h"
#include "pictor/surface/frame_result.h"
#include "pictor/surface/vulkan_context.h"
#include "test_common.h"

#include <cstdint>
#include <utility>

using namespace pictor;
using namespace pictor_test;

namespace {

MobileLifecycleController make_controller(uint32_t& flush_count) {
    MobileLifecycleController::Hooks hooks;
    hooks.current_frame = [] { return uint64_t{42}; };
    hooks.flush_frame_allocator = [&flush_count] { ++flush_count; };
    return MobileLifecycleController(std::move(hooks), {});
}

void test_surface_regain_restores_app_state() {
    {
        uint32_t flush_count = 0;
        auto controller = make_controller(flush_count);
        controller.on_surface_lost();
        controller.on_suspend();
        controller.on_surface_regained();
        PT_ASSERT(controller.snapshot().lifecycle == LifecycleState::SUSPENDED,
                  "surface regain preserves suspended app state");
        PT_ASSERT(flush_count == 0, "suspended regain does not flush frame allocator");
    }
    {
        uint32_t flush_count = 0;
        auto controller = make_controller(flush_count);
        controller.on_surface_lost();
        controller.on_pause();
        controller.on_surface_regained();
        PT_ASSERT(controller.snapshot().lifecycle == LifecycleState::PAUSED,
                  "surface regain preserves paused app state");
        PT_ASSERT(flush_count == 0, "paused regain does not flush frame allocator");
    }
    {
        uint32_t flush_count = 0;
        auto controller = make_controller(flush_count);
        controller.on_surface_lost();
        controller.on_resume();
        controller.on_surface_regained();
        PT_ASSERT(controller.snapshot().lifecycle == LifecycleState::ACTIVE,
                  "surface regain restores active app state");
        PT_ASSERT(flush_count == 1, "active regain flushes stale frame allocations");

        controller.on_surface_regained();
        PT_ASSERT(flush_count == 1, "duplicate surface regain is idempotent");
    }
}

void test_frame_result_image_contract() {
    FrameResult ready{FrameStatus::Ready, 3, true};
    PT_ASSERT(ready.has_image(), "ready acquire result exposes its image");

    FrameResult resize{FrameStatus::RecreateSwapchain, 3, true};
    PT_ASSERT(!resize.has_image(), "non-ready status never permits submission");

    VulkanContext context;
    const FrameResult acquire = context.acquire_frame();
    PT_ASSERT(acquire.status == FrameStatus::NotInitialized,
              "uninitialized acquire reports NotInitialized");
    PT_ASSERT(!acquire.has_image(), "uninitialized acquire has no image");

    const FrameResult present = context.present_frame(0);
    PT_ASSERT(present.status == FrameStatus::NotInitialized,
              "uninitialized present reports NotInitialized");
    PT_ASSERT(!present.has_image(), "present result never permits submission");
}

} // namespace

int main() {
    test_surface_regain_restores_app_state();
    test_frame_result_image_contract();
    return report("unit_frame_contract_test");
}
