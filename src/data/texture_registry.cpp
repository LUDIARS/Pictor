#include "pictor/data/texture_registry.h"

namespace pictor {

TextureRegistry::TextureRegistry(GpuMemoryAllocator& allocator)
    : allocator_(allocator)
{
}

TextureRegistry::~TextureRegistry() {
    // Free all GPU allocations
    for (auto& entry : entries_) {
        if (entry.gpu_allocation.valid) {
            allocator_.free(entry.gpu_allocation);
        }
    }
}

TextureHandle TextureRegistry::register_texture(const TextureDescriptor& desc) {
    TextureHandle handle = next_handle_++;

    TextureEntry entry;
    entry.handle       = handle;
    entry.name         = desc.name;
    entry.width        = desc.width;
    entry.height       = desc.height;
    entry.depth        = desc.depth;
    entry.mip_levels   = desc.mip_levels;
    entry.array_layers = desc.array_layers;
    entry.format       = desc.format;
    entry.type         = desc.type;
    entry.total_size   = compute_texture_size(desc);

    // Allocate GPU memory
    entry.gpu_allocation = allocator_.allocate_mesh(entry.total_size);

    // Upload initial data if provided
    if (desc.initial_data && desc.data_size > 0) {
        // Stage data for GPU upload via staging buffer
        GpuAllocation staging = allocator_.allocate_staging(desc.data_size);
        if (staging.valid) {
            // Copy source data to CPU-side buffer.
            // The Vulkan integration layer reads cpu_data to create
            // VkImage, map a staging VkBuffer, memcpy, and record
            // vkCmdCopyBufferToImage + layout transitions.
            entry.cpu_data.assign(
                static_cast<const uint8_t*>(desc.initial_data),
                static_cast<const uint8_t*>(desc.initial_data) + desc.data_size);
            entry.uploaded = true;
            allocator_.free(staging);
        }
    }

    // Register in name map
    if (!desc.name.empty()) {
        name_map_[desc.name] = handle;
    }

    entries_.push_back(std::move(entry));
    return handle;
}

bool TextureRegistry::upload_texture_data(TextureHandle handle, const void* data, size_t size,
                                          uint32_t /*mip_level*/, uint32_t /*array_layer*/) {
    if (!data || size == 0) return false;

    for (auto& entry : entries_) {
        if (entry.handle == handle) {
            GpuAllocation staging = allocator_.allocate_staging(size);
            if (!staging.valid) return false;

            // Copy to CPU-side buffer; Vulkan layer performs the actual
            // staging-buffer memcpy and vkCmdCopyBufferToImage transfer.
            entry.cpu_data.assign(
                static_cast<const uint8_t*>(data),
                static_cast<const uint8_t*>(data) + size);
            entry.uploaded = true;
            allocator_.free(staging);
            return true;
        }
    }
    return false;
}

void TextureRegistry::unregister_texture(TextureHandle handle) {
    for (auto it = entries_.begin(); it != entries_.end(); ++it) {
        if (it->handle == handle) {
            if (it->gpu_allocation.valid) {
                allocator_.free(it->gpu_allocation);
            }
            if (!it->name.empty()) {
                name_map_.erase(it->name);
            }
            // Swap-and-pop for O(1) removal
            if (it != entries_.end() - 1) {
                *it = std::move(entries_.back());
            }
            entries_.pop_back();
            return;
        }
    }
}

bool TextureRegistry::is_valid(TextureHandle handle) const {
    for (const auto& entry : entries_) {
        if (entry.handle == handle) return true;
    }
    return false;
}

const TextureEntry* TextureRegistry::get_entry(TextureHandle handle) const {
    for (const auto& entry : entries_) {
        if (entry.handle == handle) return &entry;
    }
    return nullptr;
}

TextureHandle TextureRegistry::find_by_name(const std::string& name) const {
    auto it = name_map_.find(name);
    return (it != name_map_.end()) ? it->second : INVALID_TEXTURE;
}

std::vector<TextureHandle> TextureRegistry::all_handles() const {
    std::vector<TextureHandle> handles;
    handles.reserve(entries_.size());
    for (const auto& entry : entries_) {
        handles.push_back(entry.handle);
    }
    return handles;
}

TextureRegistry::Stats TextureRegistry::get_stats() const {
    Stats stats;
    stats.texture_count = static_cast<uint32_t>(entries_.size());
    for (const auto& entry : entries_) {
        stats.total_gpu_bytes += entry.total_size;
        if (entry.uploaded) ++stats.uploaded_count;
        else ++stats.pending_count;
    }
    return stats;
}

size_t TextureRegistry::compute_texture_size(const TextureDescriptor& desc) const {
    size_t bytes_per_pixel = 0;
    switch (desc.format) {
        case TextureFormat::RGBA8_UNORM:
        case TextureFormat::RGBA8_SRGB:       bytes_per_pixel = 4; break;
        case TextureFormat::RGBA16_FLOAT:      bytes_per_pixel = 8; break;
        case TextureFormat::RGBA32_FLOAT:      bytes_per_pixel = 16; break;
        case TextureFormat::R8_UNORM:          bytes_per_pixel = 1; break;
        case TextureFormat::RG8_UNORM:         bytes_per_pixel = 2; break;
        case TextureFormat::DEPTH_32F:         bytes_per_pixel = 4; break;
        case TextureFormat::DEPTH_24_STENCIL_8: bytes_per_pixel = 4; break;
        // Block-compressed: 4x4 blocks
        case TextureFormat::BC1_UNORM:         bytes_per_pixel = 0; break; // Handled below
        case TextureFormat::BC3_UNORM:
        case TextureFormat::BC5_UNORM:
        case TextureFormat::BC7_UNORM:         bytes_per_pixel = 0; break;
        default:                               bytes_per_pixel = 4; break;
    }

    size_t total = 0;
    uint32_t w = desc.width;
    uint32_t h = desc.height;

    for (uint32_t mip = 0; mip < desc.mip_levels; ++mip) {
        size_t mip_size;
        if (bytes_per_pixel > 0) {
            mip_size = static_cast<size_t>(w) * h * bytes_per_pixel;
        } else {
            // Block-compressed: 4x4 blocks
            uint32_t bw = (w + 3) / 4;
            uint32_t bh = (h + 3) / 4;
            size_t block_size = (desc.format == TextureFormat::BC1_UNORM) ? 8 : 16;
            mip_size = static_cast<size_t>(bw) * bh * block_size;
        }
        total += mip_size * desc.array_layers * desc.depth;

        w = (w > 1) ? w / 2 : 1;
        h = (h > 1) ? h / 2 : 1;
    }

    return total;
}

} // namespace pictor
