#pragma once

#include "pictor/surface/surface_provider.h"

#include <cstdint>

namespace pictor {

/// IOSSurfaceProvider — CAMetalLayer を VkSurfaceKHR 用ハンドルに橋渡しする
/// (MoltenVK 経由)。
///
/// iOS では UIView (の layerClass = CAMetalLayer) をアプリが所有する。
/// ホストの ViewController / UIView が CAMetalLayer* を作り、
/// このプロバイダにポインタを渡す。 リサイズ (回転・SafeArea 変化) 時は
/// `update_layer()` を呼んでサイズを更新する。
///
/// metal_layer は void* で受け取る (.cpp は Objective-C を含まない)。
/// 実体は `__bridge void*` でブリッジした CAMetalLayer*。
class IOSSurfaceProvider final : public ISurfaceProvider {
public:
    IOSSurfaceProvider(void* metal_layer, uint32_t width, uint32_t height);
    ~IOSSurfaceProvider() override = default;

    NativeWindowHandle get_native_handle() const override;
    SwapchainConfig    get_swapchain_config() const override;
    void               on_swapchain_created(uint32_t width, uint32_t height) override;
    uint32_t           get_required_instance_extensions(
                           const char** out_names, uint32_t max_count) const override;

    /// CAMetalLayer の差し替え / リサイズ時にホストが呼ぶ。
    void update_layer(void* metal_layer, uint32_t width, uint32_t height);

    void* metal_layer() const { return metal_layer_; }

private:
    void*    metal_layer_ = nullptr;
    uint32_t width_       = 0;
    uint32_t height_      = 0;
};

} // namespace pictor
