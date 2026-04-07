#pragma once

#include "pictor/core/types.h"
#include "pictor/memory/gpu_memory_allocator.h"
#include <vector>
#include <string>
#include <unordered_map>

namespace pictor {

/// Descriptor for registering a texture
struct TextureDescriptor {
    std::string   name;                              // Debug/query name
    uint32_t      width        = 0;
    uint32_t      height       = 0;
    uint32_t      depth        = 1;                  // >1 for 3D textures
    uint32_t      mip_levels   = 1;
    uint32_t      array_layers = 1;                  // >1 for array textures
    TextureFormat format       = TextureFormat::RGBA8_UNORM;
    TextureType   type         = TextureType::TEXTURE_2D;
    const void*   initial_data = nullptr;            // Optional: upload immediately
    size_t        data_size    = 0;                  // Size of initial_data in bytes
};

/// GPU-side texture entry (internal bookkeeping)
struct TextureEntry {
    TextureHandle handle     = INVALID_TEXTURE;
    std::string   name;
    uint32_t      width      = 0;
    uint32_t      height     = 0;
    uint32_t      depth      = 1;
    uint32_t      mip_levels = 1;
    uint32_t      array_layers = 1;
    TextureFormat format     = TextureFormat::RGBA8_UNORM;
    TextureType   type       = TextureType::TEXTURE_2D;
    GpuAllocation gpu_allocation;
    size_t        total_size = 0;  // Total GPU memory in bytes
    bool          uploaded   = false;
    std::vector<uint8_t> cpu_data;  // CPU-side pixel data for staging upload
};

/// Texture registration and GPU memory management.
/// Supports flexible registration with deferred or immediate upload.
class TextureRegistry {
public:
    explicit TextureRegistry(GpuMemoryAllocator& allocator);
    ~TextureRegistry();

    TextureRegistry(const TextureRegistry&) = delete;
    TextureRegistry& operator=(const TextureRegistry&) = delete;

    // ---- Registration ----

    /// Register a texture. If initial_data is provided, stages it for GPU upload.
    /// Returns a TextureHandle for referencing the texture.
    TextureHandle register_texture(const TextureDescriptor& desc);

    /// Upload/replace texture data for an existing handle.
    /// Can be called repeatedly to update texture contents.
    bool upload_texture_data(TextureHandle handle, const void* data, size_t size,
                             uint32_t mip_level = 0, uint32_t array_layer = 0);

    /// Unregister a texture and free its GPU memory.
    void unregister_texture(TextureHandle handle);

    // ---- Query ----

    /// Check if a handle is valid
    bool is_valid(TextureHandle handle) const;

    /// Get texture entry (read-only). Returns nullptr if invalid.
    const TextureEntry* get_entry(TextureHandle handle) const;

    /// Find texture by name. Returns INVALID_TEXTURE if not found.
    TextureHandle find_by_name(const std::string& name) const;

    /// Get all registered texture handles
    std::vector<TextureHandle> all_handles() const;

    /// Total number of registered textures
    uint32_t count() const { return static_cast<uint32_t>(entries_.size()); }

    // ---- Stats ----

    struct Stats {
        uint32_t texture_count   = 0;
        size_t   total_gpu_bytes = 0;
        uint32_t uploaded_count  = 0;
        uint32_t pending_count   = 0;
    };

    Stats get_stats() const;

private:
    size_t compute_texture_size(const TextureDescriptor& desc) const;

    GpuMemoryAllocator& allocator_;
    std::vector<TextureEntry> entries_;
    std::unordered_map<std::string, TextureHandle> name_map_;
    TextureHandle next_handle_ = 0;
};

} // namespace pictor
