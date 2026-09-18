#pragma once

#include "pictor/surface/vulkan_device_requirements.h"
#include "pictor/xr/stereo_output_surface.h"
#include "pictor/xr/xr_session.h"

#include <memory>
#include <string>

namespace pictor::xr {

struct OpenXrRuntimeConfig {
    std::string app_name    = "Pictor";
    uint32_t    app_version = 1;
};

#ifdef PICTOR_HAS_VULKAN
/// セッションを結び付ける Vulkan 側のハンドル (借用)。 この runtime を
/// VulkanContextConfig::device_requirements に渡して作ったデバイスであること。
struct OpenXrGraphicsBinding {
    VkInstance       instance        = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device = VK_NULL_HANDLE;
    VkDevice         device          = VK_NULL_HANDLE;
    uint32_t         queue_family    = 0;
    uint32_t         queue_index     = 0;
};
#endif

/// OpenXR ランタイム (Meta Quest Link 等) との接続。 PICTOR_ENABLE_OPENXR=ON の
/// ビルドでだけ使える。 OpenXR の型は公開ヘッダへ出さない。
///
/// 初期化は 2 段階:
///   1. create()          … ランタイムへ接続し HMD を見つける
///   2. VulkanContext を、 この runtime を device_requirements にして初期化する
///   3. start_session()   … Vulkan と結び付け、 描画先と入力を用意する
///
/// 破棄順: この runtime を VkDevice より先に破棄すること (描画先の view を持つため)。
class OpenXrRuntime final : public IVulkanDeviceRequirements {
public:
    /// 失敗時は nullptr を返し、 `error` に理由を入れる (ランタイム未起動・HMD 未接続など)。
    static std::unique_ptr<OpenXrRuntime> create(const OpenXrRuntimeConfig& config,
                                                 std::string& error);
    ~OpenXrRuntime() override;

    OpenXrRuntime(const OpenXrRuntime&)            = delete;
    OpenXrRuntime& operator=(const OpenXrRuntime&) = delete;

#ifdef PICTOR_HAS_VULKAN
    bool required_instance_extensions(std::vector<std::string>& out) override;
    bool required_physical_device(VkInstance instance, VkPhysicalDevice& out) override;
    bool required_device_extensions(std::vector<std::string>& out) override;

    bool start_session(const OpenXrGraphicsBinding& binding, std::string& error);
#endif

    /// start_session() が成功するまでは nullptr。
    IXrSession*           session();
    IStereoOutputSurface* surface();

private:
    struct Impl;
    explicit OpenXrRuntime(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

} // namespace pictor::xr
