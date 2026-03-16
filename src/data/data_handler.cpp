#include "pictor/data/data_handler.h"

namespace pictor {

DataHandler::DataHandler(GpuMemoryAllocator& gpu_allocator, GPUBufferManager& buffer_manager,
                         AnimationSystem& animation_system)
    : texture_registry_(std::make_unique<TextureRegistry>(gpu_allocator))
    , vertex_uploader_(std::make_unique<VertexDataUploader>(buffer_manager))
    , model_handler_(std::make_unique<ModelDataHandler>(*vertex_uploader_, animation_system))
{
}

DataHandler::~DataHandler() = default;

// ---- Texture Operations ----

TextureHandle DataHandler::register_texture(const TextureDescriptor& desc) {
    return texture_registry_->register_texture(desc);
}

bool DataHandler::upload_texture_data(TextureHandle handle, const void* data, size_t size,
                                      uint32_t mip_level, uint32_t array_layer) {
    return texture_registry_->upload_texture_data(handle, data, size, mip_level, array_layer);
}

void DataHandler::unregister_texture(TextureHandle handle) {
    texture_registry_->unregister_texture(handle);
}

// ---- Mesh/Vertex Operations ----

MeshHandle DataHandler::register_mesh(const MeshDataDescriptor& desc) {
    return vertex_uploader_->register_mesh(desc);
}

bool DataHandler::upload_vertex_data(MeshHandle handle, const void* data, size_t size) {
    return vertex_uploader_->upload_vertex_data(handle, data, size);
}

bool DataHandler::upload_index_data(MeshHandle handle, const void* data, size_t size) {
    return vertex_uploader_->upload_index_data(handle, data, size);
}

bool DataHandler::update_vertex_region(MeshHandle handle, size_t offset,
                                        const void* data, size_t size) {
    return vertex_uploader_->update_vertex_region(handle, offset, data, size);
}

void DataHandler::unregister_mesh(MeshHandle handle) {
    vertex_uploader_->unregister_mesh(handle);
}

// ---- Model Operations ----

ModelHandle DataHandler::register_model(const ModelDescriptor& desc) {
    return model_handler_->register_model(desc);
}

void DataHandler::unregister_model(ModelHandle handle) {
    model_handler_->unregister_model(handle);
}

// ---- Stats ----

DataHandler::Stats DataHandler::get_stats() const {
    Stats stats;
    stats.texture_stats = texture_registry_->get_stats();
    stats.mesh_stats = vertex_uploader_->get_stats();
    stats.model_stats = model_handler_->get_stats();
    return stats;
}

} // namespace pictor
