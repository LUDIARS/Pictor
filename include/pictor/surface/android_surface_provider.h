#pragma once

#include "pictor/surface/surface_provider.h"

#include <cstdint>

struct ANativeWindow;

namespace pictor {

/// AndroidSurfaceProvider — ANativeWindow を VkSurfaceKHR 用ハンドルに橋渡しする。
///
/// Android では「ウィンドウ」はアプリが所有しない。 GameActivity /
/// android_native_app_glue が `onNativeWindowCreated` / `...Destroyed` で
/// ANativeWindow* を出し入れするので、 ホスト側がそれを `update_window()` で
/// 流し込む。 ウィンドウが nullptr の間 (バックグラウンド等) は
/// get_native_handle().type == None になり、 ホストは描画をスキップする。
class AndroidSurfaceProvider final : public ISurfaceProvider {
public:
    AndroidSurfaceProvider(ANativeWindow* window, uint32_t width, uint32_t height);
    ~AndroidSurfaceProvider() override = default;

    NativeWindowHandle get_native_handle() const override;
    SwapchainConfig    get_swapchain_config() const override;
    void               on_swapchain_created(uint32_t width, uint32_t height) override;
    uint32_t           get_required_instance_extensions(
                           const char** out_names, uint32_t max_count) const override;

    /// ANativeWindow の生成/破棄/サイズ変更時にホストが呼ぶ。
    /// window == nullptr で「ウィンドウなし」状態へ遷移する。
    void update_window(ANativeWindow* window, uint32_t width, uint32_t height);

    ANativeWindow* window() const { return window_; }

private:
    ANativeWindow* window_ = nullptr;
    uint32_t       width_  = 0;
    uint32_t       height_ = 0;
};

} // namespace pictor
