#pragma once

#include <string>
#include <vector>

#ifdef PICTOR_HAS_VULKAN
// Windows では <vulkan/vulkan.h> が <windows.h> を引き込み、 GDI のマクロ (OPAQUE /
// TRANSPARENT) が PassType の列挙子と衝突する。 GDI 部分を抑止する。
#ifndef NOGDI
#define NOGDI
#endif
#include <vulkan/vulkan.h>
#endif

namespace pictor {

/// Vulkan のインスタンス/デバイス生成に外部の要求を差し込む口。
///
/// XR ランタイムは「この拡張を有効にし、 この GPU を使え」と指定してくる
/// (HMD が繋がっている GPU でないと表示できないため)。 VulkanContext は生成の
/// 主導権を持ったまま、 この口から要求だけを受け取る。 XR 以外でも使える。
///
/// どのメソッドも false は「要求を取得できなかった」で、 VulkanContext は
/// 初期化を失敗させる (要求を無視して続行しない)。
class IVulkanDeviceRequirements {
public:
    virtual ~IVulkanDeviceRequirements() = default;

#ifdef PICTOR_HAS_VULKAN
    /// 追加で有効にするインスタンス拡張。
    virtual bool required_instance_extensions(std::vector<std::string>& out) = 0;

    /// 使うべき物理デバイス。 指定が無ければ `out` を VK_NULL_HANDLE のまま true を返す。
    virtual bool required_physical_device(VkInstance instance,
                                          VkPhysicalDevice& out) = 0;

    /// 追加で有効にするデバイス拡張。
    virtual bool required_device_extensions(std::vector<std::string>& out) = 0;
#endif
};

} // namespace pictor
