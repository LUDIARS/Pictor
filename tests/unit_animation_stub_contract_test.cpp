#include "pictor/animation/lottie_animation.h"
#include "pictor/animation/rive_animation.h"
#include "pictor/c_api.h"
#include "test_common.h"

#include <array>
#include <cstdint>
#include <cstring>

using namespace pictor_test;

namespace {

bool all_equal(const std::array<uint8_t, 16>& bytes, uint8_t value) {
    for (uint8_t b : bytes) {
        if (b != value) return false;
    }
    return true;
}

} // namespace

int main() {
    {
        pictor::RiveAnimation rive;
        const uint8_t fake_rive[] = {'R', 'I', 'V', 'E'};
        PT_ASSERT(rive.load_memory(fake_rive, sizeof(fake_rive)), "Rive metadata load remains available");
        PT_ASSERT(std::strstr(rive.error().c_str(), "runtime is not linked") != nullptr,
                  "Rive load reports metadata-only mode");

        std::array<uint8_t, 16> pixels;
        pixels.fill(0x7f);
        rive.render_to_buffer(pixels.data(), 2, 2);
        PT_ASSERT(all_equal(pixels, 0x7f), "Rive render does not silently clear caller buffer");
        PT_ASSERT(std::strstr(rive.error().c_str(), "unavailable") != nullptr,
                  "Rive render reports unavailable backend");
    }

    {
        pictor::LottieAnimation lottie;
        PT_ASSERT(lottie.load_json(R"({"w":2,"h":2,"ip":0,"op":30,"fr":30})"),
                  "Lottie metadata load remains available");
        PT_ASSERT(std::strstr(lottie.error().c_str(), "runtime is not linked") != nullptr,
                  "Lottie load reports metadata-only mode");

        std::array<uint8_t, 16> pixels;
        pixels.fill(0x55);
        lottie.render_to_buffer(pixels.data(), 2, 2);
        PT_ASSERT(all_equal(pixels, 0x55), "Lottie render does not silently clear caller buffer");
        PT_ASSERT(std::strstr(lottie.error().c_str(), "unavailable") != nullptr,
                  "Lottie render reports unavailable backend");
    }

    {
        PictorRenderer* renderer =
            pictor_create_renderer_vulkan(reinterpret_cast<void*>(0x1), reinterpret_cast<void*>(0x2));
        PT_ASSERT(renderer == nullptr, "C API Vulkan renderer fails explicitly");
        const char* err = pictor_last_error();
        PT_ASSERT(err != nullptr && std::strstr(err, "not implemented") != nullptr,
                  "C API Vulkan renderer reports not implemented");
    }

    return report("unit_animation_stub_contract_test");
}
