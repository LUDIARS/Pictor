#include "sp_kirie_backdrop.h"

#include <stb_image.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace sp_demo {

using pictor::VulkanContext;

namespace {

/// 台紙キーイング: 四隅の平均色を紙地とみなし、色距離をアルファへ変換する。
/// dist <= t0 で完全透明、t1 以上で不透明 (間は線形)。
/// @implements SPEC-SHADOW-KIRIE-BACKDROP
void key_out_background(std::vector<unsigned char>& rgba, int w, int h) {
    auto pixel = [&](int x, int y) { return &rgba[(size_t)(y * w + x) * 4]; };

    float bg[3] = {0.0f, 0.0f, 0.0f};
    const int corners[4][2] = {{0, 0}, {w - 1, 0}, {0, h - 1}, {w - 1, h - 1}};
    for (auto& c : corners) {
        const unsigned char* p = pixel(c[0], c[1]);
        bg[0] += p[0]; bg[1] += p[1]; bg[2] += p[2];
    }
    bg[0] *= 0.25f; bg[1] *= 0.25f; bg[2] *= 0.25f;

    // 台紙 (紙テクスチャ) のノイズは色距離 ~43 以内、切り絵ピースは ~90 以上
    // に分離している (gen08 実測)。間を線形の縁として使う。
    const float t0 = 50.0f;
    const float t1 = 85.0f;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            unsigned char* p = pixel(x, y);
            float dr = p[0] - bg[0], dg = p[1] - bg[1], db = p[2] - bg[2];
            float dist  = std::sqrt(dr * dr + dg * dg + db * db);
            float alpha = (dist - t0) / (t1 - t0);
            alpha = alpha < 0.0f ? 0.0f : (alpha > 1.0f ? 1.0f : alpha);
            p[3] = (unsigned char)(alpha * 255.0f + 0.5f);
        }
    }

    // 画像最下段に残る別ピースの断片を落とす (鷹本体は上側に収まっている)
    const int crop_from = (int)(h * 0.88f);
    for (int y = crop_from; y < h; ++y)
        for (int x = 0; x < w; ++x)
            pixel(x, y)[3] = 0;
}

} // namespace

/// @implements SPEC-SHADOW-KIRIE-BACKDROP
bool SpKirieBackdrop::initialize(VulkanContext& vk_ctx, const char* asset_dir) {
    shutdown();
    vk_ctx_ = &vk_ctx;
    device_ = vk_ctx.device();
    if (!device_ || !asset_dir || !asset_dir[0]) {
        shutdown();
        return false;
    }

    VkSamplerCreateInfo samp{};
    samp.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samp.magFilter    = VK_FILTER_LINEAR;
    samp.minFilter    = VK_FILTER_LINEAR;
    samp.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samp.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samp.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (vkCreateSampler(device_, &samp, nullptr, &sampler_) != VK_SUCCESS) {
        shutdown();
        return false;
    }

    std::string base = std::string(asset_dir) + "/";
    bool town_ok = load_png_texture((base + "kirie_town.png").c_str(), false, town_);
    bool hawk_ok = load_png_texture((base + "kirie_hawk.png").c_str(), true, hawk_);
    if (!town_ok || !hawk_ok) {
        // フォールバック: 1x1 白 (α=0)。デスクリプタを有効に保ちつつ
        // シェーダ側の合成は no-op になる。
        const unsigned char white[4] = {255, 255, 255, 0};
        if ((!town_ok && !create_texture_rgba(white, 1, 1, town_)) ||
            (!hawk_ok && !create_texture_rgba(white, 1, 1, hawk_))) {
            shutdown();
            return false;
        }
        fprintf(stderr,
                "SpKirieBackdrop: assets not found under %s (town=%d hawk=%d) — "
                "falling back to blank backdrop\n",
                asset_dir, (int)town_ok, (int)hawk_ok);
    }
    return true;
}

/// @implements SPEC-SHADOW-KIRIE-BACKDROP
void SpKirieBackdrop::shutdown() {
    if (device_) {
        destroy_texture(town_);
        destroy_texture(hawk_);
        if (sampler_) vkDestroySampler(device_, sampler_, nullptr);
    }
    town_ = {};
    hawk_ = {};
    sampler_ = VK_NULL_HANDLE;
    device_ = VK_NULL_HANDLE;
    vk_ctx_ = nullptr;
}

/// @implements SPEC-SHADOW-KIRIE-BACKDROP
bool SpKirieBackdrop::load_png_texture(const char* path, bool key_background,
                                       Texture& out) {
    int w = 0, h = 0, channels = 0;
    unsigned char* data = stbi_load(path, &w, &h, &channels, 4); // RGBA 強制
    if (!data) return false;

    const size_t width = w > 0 ? static_cast<size_t>(w) : 0;
    const size_t height = h > 0 ? static_cast<size_t>(h) : 0;
    if (width == 0 || height == 0 ||
        width > STBI_MAX_DIMENSIONS || height > STBI_MAX_DIMENSIONS ||
        width > std::numeric_limits<size_t>::max() / height / 4) {
        stbi_image_free(data);
        return false;
    }

    const size_t byte_count = width * height * 4;
    std::vector<unsigned char> rgba(data, data + byte_count);
    stbi_image_free(data);

    if (key_background) {
        key_out_background(rgba, w, h);
    } else {
        for (size_t i = 3; i < rgba.size(); i += 4) rgba[i] = 255;
    }

    if (!create_texture_rgba(rgba.data(), (uint32_t)w, (uint32_t)h, out))
        return false;
    printf("SpKirieBackdrop: loaded %s (%dx%d)\n", path, w, h);
    return true;
}

/// @implements SPEC-SHADOW-KIRIE-BACKDROP
bool SpKirieBackdrop::create_texture_rgba(const unsigned char* rgba,
                                          uint32_t width, uint32_t height,
                                          Texture& out) {
    if (!rgba || width == 0 || height == 0 ||
        width > std::numeric_limits<VkDeviceSize>::max() / height / 4) {
        return false;
    }

    VkDeviceSize image_size = (VkDeviceSize)width * height * 4;

    // ---- ステージングバッファ ----
    VkBuffer       staging_buf = VK_NULL_HANDLE;
    VkDeviceMemory staging_mem = VK_NULL_HANDLE;
    {
        VkBufferCreateInfo buf_info{};
        buf_info.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        buf_info.size        = image_size;
        buf_info.usage       = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        buf_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateBuffer(device_, &buf_info, nullptr, &staging_buf) != VK_SUCCESS)
            return false;

        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(device_, staging_buf, &req);
        VkMemoryAllocateInfo alloc{};
        alloc.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        alloc.allocationSize  = req.size;
        if (!find_memory_type(req.memoryTypeBits,
                              VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                  VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                              alloc.memoryTypeIndex) ||
            vkAllocateMemory(device_, &alloc, nullptr, &staging_mem) != VK_SUCCESS) {
            vkDestroyBuffer(device_, staging_buf, nullptr);
            return false;
        }
        if (vkBindBufferMemory(device_, staging_buf, staging_mem, 0) != VK_SUCCESS) {
            vkDestroyBuffer(device_, staging_buf, nullptr);
            vkFreeMemory(device_, staging_mem, nullptr);
            return false;
        }

        void* mapped = nullptr;
        if (vkMapMemory(device_, staging_mem, 0, image_size, 0, &mapped) != VK_SUCCESS) {
            vkDestroyBuffer(device_, staging_buf, nullptr);
            vkFreeMemory(device_, staging_mem, nullptr);
            return false;
        }
        memcpy(mapped, rgba, (size_t)image_size);
        vkUnmapMemory(device_, staging_mem);
    }

    auto cleanup_staging = [&]() {
        vkDestroyBuffer(device_, staging_buf, nullptr);
        vkFreeMemory(device_, staging_mem, nullptr);
    };

    // ---- イメージ ----
    Texture texture{};
    auto cleanup_texture = [&]() { destroy_texture(texture); };

    VkImageCreateInfo img_info{};
    img_info.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    img_info.imageType     = VK_IMAGE_TYPE_2D;
    img_info.format        = VK_FORMAT_R8G8B8A8_UNORM;
    img_info.extent        = {width, height, 1};
    img_info.mipLevels     = 1;
    img_info.arrayLayers   = 1;
    img_info.samples       = VK_SAMPLE_COUNT_1_BIT;
    img_info.tiling        = VK_IMAGE_TILING_OPTIMAL;
    img_info.usage         = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    img_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(device_, &img_info, nullptr, &texture.image) != VK_SUCCESS) {
        cleanup_staging();
        return false;
    }

    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(device_, texture.image, &req);
    VkMemoryAllocateInfo alloc{};
    alloc.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize  = req.size;
    if (!find_memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                          alloc.memoryTypeIndex) ||
        vkAllocateMemory(device_, &alloc, nullptr, &texture.memory) != VK_SUCCESS) {
        cleanup_staging();
        cleanup_texture();
        return false;
    }
    if (vkBindImageMemory(device_, texture.image, texture.memory, 0) != VK_SUCCESS) {
        cleanup_staging();
        cleanup_texture();
        return false;
    }

    // ---- 転送 (UNDEFINED → TRANSFER_DST → SHADER_READ_ONLY) ----
    VkCommandBuffer cmd = vk_ctx_->begin_single_time_commands();
    if (!cmd) {
        cleanup_staging();
        cleanup_texture();
        return false;
    }

    VkImageMemoryBarrier barrier{};
    barrier.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image               = texture.image;
    barrier.subresourceRange    = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    barrier.srcAccessMask       = 0;
    barrier.dstAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &barrier);

    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent      = {width, height, 1};
    vkCmdCopyBufferToImage(cmd, staging_buf, texture.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    barrier.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &barrier);

    vk_ctx_->end_single_time_commands(cmd);
    cleanup_staging();

    // ---- ビュー ----
    VkImageViewCreateInfo view_info{};
    view_info.sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.image            = texture.image;
    view_info.viewType         = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format           = VK_FORMAT_R8G8B8A8_UNORM;
    view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (vkCreateImageView(device_, &view_info, nullptr, &texture.view) != VK_SUCCESS) {
        cleanup_texture();
        return false;
    }

    out = texture;
    return true;
}

/// @implements SPEC-SHADOW-KIRIE-BACKDROP
void SpKirieBackdrop::destroy_texture(Texture& tex) {
    if (tex.view)   vkDestroyImageView(device_, tex.view, nullptr);
    if (tex.image)  vkDestroyImage(device_, tex.image, nullptr);
    if (tex.memory) vkFreeMemory(device_, tex.memory, nullptr);
    tex = {};
}

/// @implements SPEC-SHADOW-KIRIE-BACKDROP
bool SpKirieBackdrop::find_memory_type(uint32_t type_filter,
                                       VkMemoryPropertyFlags props,
                                       uint32_t& memory_type) const {
    VkPhysicalDeviceMemoryProperties mem_props;
    vkGetPhysicalDeviceMemoryProperties(vk_ctx_->physical_device(), &mem_props);
    for (uint32_t i = 0; i < mem_props.memoryTypeCount; ++i) {
        if ((type_filter & (1u << i)) &&
            (mem_props.memoryTypes[i].propertyFlags & props) == props) {
            memory_type = i;
            return true;
        }
    }
    return false;
}

} // namespace sp_demo
