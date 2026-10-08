#include "channel_list.h"

#include "pictor/animation/skeleton.h"

namespace pictor_fbx_viewer {

namespace {

void put_json_string(std::FILE* out, const std::string& s) {
    std::fputc('"', out);
    for (unsigned char c : s) {
        switch (c) {
            case '"':  std::fputs("\\\"", out); break;
            case '\\': std::fputs("\\\\", out); break;
            case '\n': std::fputs("\\n", out);  break;
            case '\r': std::fputs("\\r", out);  break;
            case '\t': std::fputs("\\t", out);  break;
            default:
                if (c < 0x20) std::fprintf(out, "\\u%04x", c);
                else std::fputc(c, out);
        }
    }
    std::fputc('"', out);
}

} // namespace

/// @implements SPEC-PC-FBX-TRACK-PLAYBACK
bool write_channel_list_json(std::FILE* out, const pictor::SkeletonDescriptor& skeleton,
                             const std::vector<std::string>& shape_names) {
    const pictor::Skeleton skel(skeleton);
    const uint32_t n = skel.bone_count();
    std::vector<pictor::Transform> local(n);
    std::vector<pictor::float4x4>  world(n);
    if (n > 0) {
        skel.get_bind_pose(local.data());
        skel.compute_world_matrices(local.data(), world.data());
    }

    std::fputs("{\n  \"bones\": [", out);
    for (uint32_t i = 0; i < n; ++i) {
        const pictor::Bone& b = skel.bone(i);
        std::fputs(i ? ",\n    {\"name\": " : "\n    {\"name\": ", out);
        put_json_string(out, b.name);
        std::fputs(", \"parent\": ", out);
        if (b.parent_index >= 0 && static_cast<uint32_t>(b.parent_index) < n)
            put_json_string(out, skel.bone(static_cast<uint32_t>(b.parent_index)).name);
        else
            std::fputs("null", out);
        // Row-vector matrices: the world translation (bone head) is row 3.
        std::fprintf(out, ", \"head\": [%.6g, %.6g, %.6g]}",
                     world[i].m[3][0], world[i].m[3][1], world[i].m[3][2]);
    }
    std::fputs(n ? "\n  ],\n  \"blendshapes\": [" : "],\n  \"blendshapes\": [", out);
    for (size_t i = 0; i < shape_names.size(); ++i) {
        std::fputs(i ? ",\n    " : "\n    ", out);
        put_json_string(out, shape_names[i]);
    }
    std::fputs(shape_names.empty() ? "]\n}\n" : "\n  ]\n}\n", out);
    return std::fflush(out) == 0 && std::ferror(out) == 0;
}

} // namespace pictor_fbx_viewer
