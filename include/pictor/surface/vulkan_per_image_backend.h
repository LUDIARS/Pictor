#pragma once

#ifdef PICTOR_HAS_VULKAN

#include <cstdint>
#include <vulkan/vulkan.h>

namespace pictor {

/// PerImageResourcePool の Vulkan 実装 policy。
///
/// 責務は「Vulkan API を叩いて per-image リソースを 1 個作る / 壊す」だけで、
/// 何個作るか・どの順で解放するかは PerImageResourcePool 側が決める。
/// device / command pool は借用 (VulkanContext が所有)。
class VulkanPerImageBackend {
public:
    using CommandBuffer = VkCommandBuffer;
    using Semaphore     = VkSemaphore;
    using Fence         = VkFence;

    VulkanPerImageBackend() = default;
    VulkanPerImageBackend(VkDevice device, VkCommandPool pool)
        : device_(device), pool_(pool) {}

    static CommandBuffer null_command_buffer() { return VK_NULL_HANDLE; }
    static Semaphore     null_semaphore()      { return VK_NULL_HANDLE; }
    static Fence         null_fence()          { return VK_NULL_HANDLE; }

    /// command buffer を扱えるか (device + command pool の両方が要る)。
    bool valid() const { return device_ != VK_NULL_HANDLE && pool_ != VK_NULL_HANDLE; }
    /// セマフォを扱えるか。 セマフォは command pool と無関係なので device だけ見る。
    /// pool を見てしまうと、 pool が未生成 / 破棄済みの経路で destroy_semaphore()
    /// が黙って何もせず、 セマフォが漏れる。
    bool device_valid() const { return device_ != VK_NULL_HANDLE; }

    bool allocate_command_buffers(uint32_t count, CommandBuffer* out) {
        if (!valid() || count == 0 || !out) return false;
        VkCommandBufferAllocateInfo alloc_info{};
        alloc_info.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        alloc_info.commandPool        = pool_;
        alloc_info.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        alloc_info.commandBufferCount = count;
        return vkAllocateCommandBuffers(device_, &alloc_info, out) == VK_SUCCESS;
    }

    void free_command_buffers(const CommandBuffer* first, uint32_t count) {
        if (!valid() || count == 0 || !first) return;
        vkFreeCommandBuffers(device_, pool_, count, first);
    }

    bool create_semaphore(Semaphore* out) {
        if (!device_valid() || !out) return false;
        VkSemaphoreCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        return vkCreateSemaphore(device_, &info, nullptr, out) == VK_SUCCESS;
    }

    void destroy_semaphore(Semaphore sem) {
        if (!device_valid() || sem == VK_NULL_HANDLE) return;
        vkDestroySemaphore(device_, sem, nullptr);
    }

private:
    VkDevice      device_ = VK_NULL_HANDLE;  // 借用
    VkCommandPool pool_   = VK_NULL_HANDLE;  // 借用
};

} // namespace pictor

#endif // PICTOR_HAS_VULKAN
