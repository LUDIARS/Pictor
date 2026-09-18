#include "pictor/xr/openxr_runtime.h"

#include "openxr_common.h"
#include "openxr_session.h"
#include "openxr_swapchain_surface.h"

#include <cstring>
#include <sstream>

namespace pictor::xr {

using namespace detail;

namespace {

/// XR_KHR_vulkan_enable の関数。 拡張の関数はローダが公開しないので実行時に引く。
struct VulkanEnableFunctions {
    PFN_xrGetVulkanInstanceExtensionsKHR   get_instance_extensions   = nullptr;
    PFN_xrGetVulkanDeviceExtensionsKHR     get_device_extensions     = nullptr;
    PFN_xrGetVulkanGraphicsDeviceKHR       get_graphics_device       = nullptr;
    PFN_xrGetVulkanGraphicsRequirementsKHR get_graphics_requirements = nullptr;
};

template <typename Fn>
bool load_function(XrInstance instance, const char* name, Fn& out) {
    PFN_xrVoidFunction fn = nullptr;
    if (!xr_check(xrGetInstanceProcAddr(instance, name, &fn), name) || !fn) return false;
    out = reinterpret_cast<Fn>(fn);
    return true;
}

bool is_extension_available(const char* name) {
    uint32_t count = 0;
    if (XR_FAILED(xrEnumerateInstanceExtensionProperties(nullptr, 0, &count, nullptr))) return false;
    XrExtensionProperties blank{XR_TYPE_EXTENSION_PROPERTIES};
    std::vector<XrExtensionProperties> props(count, blank);
    if (XR_FAILED(xrEnumerateInstanceExtensionProperties(nullptr, count, &count, props.data())))
        return false;
    for (const XrExtensionProperties& p : props) {
        if (std::strcmp(p.extensionName, name) == 0) return true;
    }
    return false;
}

/// OpenXR が返す「空白区切りの拡張名」を 1 つずつに分ける。
void split_names(const std::string& joined, std::vector<std::string>& out) {
    std::istringstream stream(joined);
    std::string name;
    while (stream >> name) out.push_back(name);
}

} // namespace

struct OpenXrRuntime::Impl {
    XrInstance            instance = XR_NULL_HANDLE;
    XrSystemId            system   = XR_NULL_SYSTEM_ID;
    VulkanEnableFunctions vk;
    EyeExtent             recommended;

    // 破棄順は surface (swapchain) → session → instance。 デストラクタで明示する。
    OpenXrSession          session;
    OpenXrSwapchainSurface surface;
    bool                   is_session_started = false;

    ~Impl() {
        surface.shutdown();
        session.shutdown();
        if (instance != XR_NULL_HANDLE) xrDestroyInstance(instance);
    }

    /// 拡張名の一覧を 2 回呼び (長さ → 本体) で取る共通手順。
    template <typename Fn>
    bool query_names(Fn fn, const char* what, std::vector<std::string>& out) {
        uint32_t size = 0;
        if (!xr_check(fn(instance, system, 0, &size, nullptr), what)) return false;
        std::string joined(size, '\0');
        if (!xr_check(fn(instance, system, size, &size, joined.data()), what)) return false;
        joined.resize(std::strlen(joined.c_str()));
        split_names(joined, out);
        return true;
    }
};

OpenXrRuntime::OpenXrRuntime(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
OpenXrRuntime::~OpenXrRuntime() = default;

std::unique_ptr<OpenXrRuntime> OpenXrRuntime::create(const OpenXrRuntimeConfig& config,
                                                     std::string& error) {
    if (!is_extension_available(XR_KHR_VULKAN_ENABLE_EXTENSION_NAME)) {
        error = "OpenXR ランタイムが見つからないか、 Vulkan に対応していません "
                "(ランタイムが起動していて、 既定の OpenXR ランタイムに設定されているか確認)";
        return nullptr;
    }

    auto impl = std::make_unique<Impl>();

    const char* extensions[] = {XR_KHR_VULKAN_ENABLE_EXTENSION_NAME};
    XrInstanceCreateInfo info{XR_TYPE_INSTANCE_CREATE_INFO};
    copy_name(info.applicationInfo.applicationName, config.app_name.c_str());
    copy_name(info.applicationInfo.engineName, "Pictor");
    info.applicationInfo.applicationVersion = config.app_version;
    info.applicationInfo.apiVersion         = XR_API_VERSION_1_0;
    info.enabledExtensionCount              = 1;
    info.enabledExtensionNames              = extensions;
    if (!xr_check(xrCreateInstance(&info, &impl->instance), "xrCreateInstance")) {
        impl->instance = XR_NULL_HANDLE;
        error = "OpenXR インスタンスを作成できません";
        return nullptr;
    }

    XrSystemGetInfo system_info{XR_TYPE_SYSTEM_GET_INFO};
    system_info.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    if (!xr_check(xrGetSystem(impl->instance, &system_info, &impl->system), "xrGetSystem")) {
        error = "HMD が見つかりません (接続と Link の状態を確認)";
        return nullptr;
    }

    if (!load_function(impl->instance, "xrGetVulkanInstanceExtensionsKHR",
                       impl->vk.get_instance_extensions) ||
        !load_function(impl->instance, "xrGetVulkanDeviceExtensionsKHR",
                       impl->vk.get_device_extensions) ||
        !load_function(impl->instance, "xrGetVulkanGraphicsDeviceKHR",
                       impl->vk.get_graphics_device) ||
        !load_function(impl->instance, "xrGetVulkanGraphicsRequirementsKHR",
                       impl->vk.get_graphics_requirements)) {
        error = "XR_KHR_vulkan_enable の関数を取得できません";
        return nullptr;
    }

    uint32_t view_count = 0;
    XrViewConfigurationView blank{XR_TYPE_VIEW_CONFIGURATION_VIEW};
    XrViewConfigurationView views[kEyeCount] = {blank, blank};
    if (!xr_check(xrEnumerateViewConfigurationViews(
                      impl->instance, impl->system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                      kEyeCount, &view_count, views),
                  "xrEnumerateViewConfigurationViews") ||
        view_count != kEyeCount) {
        error = "HMD が両眼の表示構成を返しません";
        return nullptr;
    }
    impl->recommended = {views[0].recommendedImageRectWidth, views[0].recommendedImageRectHeight};

    return std::unique_ptr<OpenXrRuntime>(new OpenXrRuntime(std::move(impl)));
}

bool OpenXrRuntime::required_instance_extensions(std::vector<std::string>& out) {
    return impl_->query_names(impl_->vk.get_instance_extensions,
                              "xrGetVulkanInstanceExtensionsKHR", out);
}

bool OpenXrRuntime::required_device_extensions(std::vector<std::string>& out) {
    return impl_->query_names(impl_->vk.get_device_extensions,
                              "xrGetVulkanDeviceExtensionsKHR", out);
}

bool OpenXrRuntime::required_physical_device(VkInstance instance, VkPhysicalDevice& out) {
    return xr_check(impl_->vk.get_graphics_device(impl_->instance, impl_->system, instance, &out),
                    "xrGetVulkanGraphicsDeviceKHR");
}

bool OpenXrRuntime::start_session(const OpenXrGraphicsBinding& binding, std::string& error) {
    if (impl_->is_session_started) {
        error = "セッションは開始済みです";
        return false;
    }

    // セッション作成の前に必ず呼ぶ決まり (呼ばないと xrCreateSession が失敗する)。
    XrGraphicsRequirementsVulkanKHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR};
    if (!xr_check(impl_->vk.get_graphics_requirements(impl_->instance, impl_->system,
                                                      &requirements),
                  "xrGetVulkanGraphicsRequirementsKHR")) {
        error = "Vulkan の要件を取得できません";
        return false;
    }

    XrGraphicsBindingVulkanKHR xr_binding{XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR};
    xr_binding.instance         = binding.instance;
    xr_binding.physicalDevice   = binding.physical_device;
    xr_binding.device           = binding.device;
    xr_binding.queueFamilyIndex = binding.queue_family;
    xr_binding.queueIndex       = binding.queue_index;

    if (!impl_->session.initialize(impl_->instance, impl_->system, xr_binding,
                                   impl_->recommended)) {
        error = "OpenXR セッションを作成できません";
        return false;
    }
    if (!impl_->surface.initialize(impl_->session.handle(), binding.device, impl_->recommended)) {
        impl_->session.shutdown();
        error = "HMD 用の描画先を作成できません";
        return false;
    }
    impl_->session.set_projection_target({impl_->surface.handle(), impl_->surface.eye_extent()});
    impl_->is_session_started = true;
    return true;
}

IXrSession* OpenXrRuntime::session() {
    return impl_->is_session_started ? &impl_->session : nullptr;
}

IStereoOutputSurface* OpenXrRuntime::surface() {
    return impl_->is_session_started ? &impl_->surface : nullptr;
}

} // namespace pictor::xr
