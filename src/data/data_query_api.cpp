#include "pictor/data/data_query_api.h"
#include <sstream>

namespace pictor {

DataQueryAPI::DataQueryAPI(const DataHandler& handler)
    : handler_(handler)
{
}

DataQueryAPI::~DataQueryAPI() = default;

// ---- Summary ----

DataSummary DataQueryAPI::get_summary() const {
    DataSummary summary;
    auto tex_stats = handler_.textures().get_stats();
    auto mesh_stats = handler_.meshes().get_stats();

    summary.total_textures        = tex_stats.texture_count;
    summary.total_meshes          = mesh_stats.mesh_count;
    summary.total_texture_gpu_bytes = tex_stats.total_gpu_bytes;
    summary.total_mesh_gpu_bytes  = mesh_stats.total_vertex_bytes + mesh_stats.total_index_bytes;
    summary.textures_uploaded     = tex_stats.uploaded_count;
    summary.meshes_uploaded       = mesh_stats.uploaded_count;
    return summary;
}

// ---- Texture Queries ----

TextureInfo DataQueryAPI::get_texture_info(TextureHandle handle) const {
    TextureInfo info;
    const auto* entry = handler_.textures().get_entry(handle);
    if (!entry) return info;

    info.handle       = entry->handle;
    info.name         = entry->name;
    info.width        = entry->width;
    info.height       = entry->height;
    info.depth        = entry->depth;
    info.mip_levels   = entry->mip_levels;
    info.array_layers = entry->array_layers;
    info.format       = entry->format;
    info.type         = entry->type;
    info.gpu_bytes    = entry->total_size;
    info.uploaded     = entry->uploaded;
    return info;
}

std::vector<TextureInfo> DataQueryAPI::get_all_textures() const {
    std::vector<TextureInfo> result;
    auto handles = handler_.textures().all_handles();
    result.reserve(handles.size());
    for (auto h : handles) {
        result.push_back(get_texture_info(h));
    }
    return result;
}

TextureInfo DataQueryAPI::find_texture(const std::string& name) const {
    TextureHandle handle = handler_.textures().find_by_name(name);
    return get_texture_info(handle);
}

void DataQueryAPI::for_each_texture(std::function<void(const TextureInfo&)> callback) const {
    auto handles = handler_.textures().all_handles();
    for (auto h : handles) {
        callback(get_texture_info(h));
    }
}

// ---- Mesh Queries ----

MeshInfo DataQueryAPI::get_mesh_info(MeshHandle handle) const {
    MeshInfo info;
    const auto* entry = handler_.meshes().get_entry(handle);
    if (!entry) return info;

    info.handle       = entry->handle;
    info.name         = entry->name;
    info.layout       = entry->layout;
    info.vertex_count = entry->vertex_count;
    info.index_count  = entry->index_count;
    info.index_32bit  = entry->index_32bit;
    info.vertex_bytes = entry->gpu_allocation.vertex_buffer.size;
    info.index_bytes  = entry->gpu_allocation.index_buffer.size;
    info.uploaded     = entry->uploaded;
    return info;
}

std::vector<MeshInfo> DataQueryAPI::get_all_meshes() const {
    std::vector<MeshInfo> result;
    auto handles = handler_.meshes().all_handles();
    result.reserve(handles.size());
    for (auto h : handles) {
        result.push_back(get_mesh_info(h));
    }
    return result;
}

MeshInfo DataQueryAPI::find_mesh(const std::string& name) const {
    MeshHandle handle = handler_.meshes().find_by_name(name);
    return get_mesh_info(handle);
}

void DataQueryAPI::for_each_mesh(std::function<void(const MeshInfo&)> callback) const {
    auto handles = handler_.meshes().all_handles();
    for (auto h : handles) {
        callback(get_mesh_info(h));
    }
}

// ---- Filtering ----

std::vector<TextureInfo> DataQueryAPI::get_textures_by_format(TextureFormat format) const {
    std::vector<TextureInfo> result;
    auto handles = handler_.textures().all_handles();
    for (auto h : handles) {
        auto info = get_texture_info(h);
        if (info.format == format) {
            result.push_back(std::move(info));
        }
    }
    return result;
}

std::vector<MeshInfo> DataQueryAPI::get_meshes_by_semantic(VertexSemantic semantic) const {
    std::vector<MeshInfo> result;
    auto handles = handler_.meshes().all_handles();
    for (auto h : handles) {
        auto info = get_mesh_info(h);
        for (const auto& attr : info.layout.attributes) {
            if (attr.semantic == semantic) {
                result.push_back(std::move(info));
                break;
            }
        }
    }
    return result;
}

// ---- JSON Export ----

namespace {

const char* texture_format_name(TextureFormat f) {
    switch (f) {
        case TextureFormat::RGBA8_UNORM:        return "RGBA8_UNORM";
        case TextureFormat::RGBA8_SRGB:         return "RGBA8_SRGB";
        case TextureFormat::RGBA16_FLOAT:       return "RGBA16_FLOAT";
        case TextureFormat::RGBA32_FLOAT:       return "RGBA32_FLOAT";
        case TextureFormat::R8_UNORM:           return "R8_UNORM";
        case TextureFormat::RG8_UNORM:          return "RG8_UNORM";
        case TextureFormat::BC1_UNORM:          return "BC1_UNORM";
        case TextureFormat::BC3_UNORM:          return "BC3_UNORM";
        case TextureFormat::BC5_UNORM:          return "BC5_UNORM";
        case TextureFormat::BC7_UNORM:          return "BC7_UNORM";
        case TextureFormat::DEPTH_32F:          return "DEPTH_32F";
        case TextureFormat::DEPTH_24_STENCIL_8: return "DEPTH_24_STENCIL_8";
        default:                                return "UNKNOWN";
    }
}

const char* texture_type_name(TextureType t) {
    switch (t) {
        case TextureType::TEXTURE_2D:       return "2D";
        case TextureType::TEXTURE_3D:       return "3D";
        case TextureType::TEXTURE_CUBE:     return "Cube";
        case TextureType::TEXTURE_2D_ARRAY: return "2DArray";
        default:                            return "UNKNOWN";
    }
}

const char* vertex_semantic_name(VertexSemantic s) {
    switch (s) {
        case VertexSemantic::POSITION:  return "POSITION";
        case VertexSemantic::NORMAL:    return "NORMAL";
        case VertexSemantic::TANGENT:   return "TANGENT";
        case VertexSemantic::TEXCOORD0: return "TEXCOORD0";
        case VertexSemantic::TEXCOORD1: return "TEXCOORD1";
        case VertexSemantic::COLOR0:    return "COLOR0";
        case VertexSemantic::JOINTS:    return "JOINTS";
        case VertexSemantic::WEIGHTS:   return "WEIGHTS";
        case VertexSemantic::CUSTOM0:   return "CUSTOM0";
        case VertexSemantic::CUSTOM1:   return "CUSTOM1";
        case VertexSemantic::CUSTOM2:   return "CUSTOM2";
        case VertexSemantic::CUSTOM3:   return "CUSTOM3";
        default:                        return "UNKNOWN";
    }
}

const char* vertex_attr_type_name(VertexAttributeType t) {
    switch (t) {
        case VertexAttributeType::FLOAT:    return "FLOAT";
        case VertexAttributeType::FLOAT2:   return "FLOAT2";
        case VertexAttributeType::FLOAT3:   return "FLOAT3";
        case VertexAttributeType::FLOAT4:   return "FLOAT4";
        case VertexAttributeType::UINT32:   return "UINT32";
        case VertexAttributeType::INT32:    return "INT32";
        case VertexAttributeType::UNORM8X4: return "UNORM8X4";
        case VertexAttributeType::HALF2:    return "HALF2";
        case VertexAttributeType::HALF4:    return "HALF4";
        default:                            return "UNKNOWN";
    }
}

} // anonymous namespace

std::string DataQueryAPI::export_json() const {
    std::ostringstream ss;
    ss << "{\n";
    ss << "  \"textures\": " << export_textures_json() << ",\n";
    ss << "  \"meshes\": " << export_meshes_json() << "\n";
    ss << "}";
    return ss.str();
}

std::string DataQueryAPI::export_textures_json() const {
    std::ostringstream ss;
    auto textures = get_all_textures();

    ss << "[\n";
    for (size_t i = 0; i < textures.size(); ++i) {
        const auto& t = textures[i];
        ss << "    {\n";
        ss << "      \"handle\": " << t.handle << ",\n";
        ss << "      \"name\": \"" << t.name << "\",\n";
        ss << "      \"width\": " << t.width << ",\n";
        ss << "      \"height\": " << t.height << ",\n";
        ss << "      \"depth\": " << t.depth << ",\n";
        ss << "      \"mip_levels\": " << t.mip_levels << ",\n";
        ss << "      \"array_layers\": " << t.array_layers << ",\n";
        ss << "      \"format\": \"" << texture_format_name(t.format) << "\",\n";
        ss << "      \"type\": \"" << texture_type_name(t.type) << "\",\n";
        ss << "      \"gpu_bytes\": " << t.gpu_bytes << ",\n";
        ss << "      \"uploaded\": " << (t.uploaded ? "true" : "false") << "\n";
        ss << "    }";
        if (i + 1 < textures.size()) ss << ",";
        ss << "\n";
    }
    ss << "  ]";
    return ss.str();
}

std::string DataQueryAPI::export_meshes_json() const {
    std::ostringstream ss;
    auto meshes = get_all_meshes();

    ss << "[\n";
    for (size_t i = 0; i < meshes.size(); ++i) {
        const auto& m = meshes[i];
        ss << "    {\n";
        ss << "      \"handle\": " << m.handle << ",\n";
        ss << "      \"name\": \"" << m.name << "\",\n";
        ss << "      \"vertex_count\": " << m.vertex_count << ",\n";
        ss << "      \"index_count\": " << m.index_count << ",\n";
        ss << "      \"index_32bit\": " << (m.index_32bit ? "true" : "false") << ",\n";
        ss << "      \"vertex_bytes\": " << m.vertex_bytes << ",\n";
        ss << "      \"index_bytes\": " << m.index_bytes << ",\n";
        ss << "      \"uploaded\": " << (m.uploaded ? "true" : "false") << ",\n";
        ss << "      \"stride\": " << m.layout.computed_stride() << ",\n";
        ss << "      \"attributes\": [\n";
        for (size_t j = 0; j < m.layout.attributes.size(); ++j) {
            const auto& a = m.layout.attributes[j];
            ss << "        {\n";
            ss << "          \"semantic\": \"" << vertex_semantic_name(a.semantic) << "\",\n";
            ss << "          \"type\": \"" << vertex_attr_type_name(a.type) << "\",\n";
            ss << "          \"offset\": " << a.offset << "\n";
            ss << "        }";
            if (j + 1 < m.layout.attributes.size()) ss << ",";
            ss << "\n";
        }
        ss << "      ]\n";
        ss << "    }";
        if (i + 1 < meshes.size()) ss << ",";
        ss << "\n";
    }
    ss << "  ]";
    return ss.str();
}

} // namespace pictor
