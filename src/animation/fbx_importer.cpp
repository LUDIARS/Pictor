#include "pictor/animation/fbx_importer.h"
#include <fstream>
#include <cstring>
#include <sstream>
#include <algorithm>

namespace pictor {

// FBX binary magic bytes: "Kaydara FBX Binary  \0"
static constexpr char FBX_BINARY_MAGIC[] = "Kaydara FBX Binary  ";
static constexpr size_t FBX_BINARY_MAGIC_LEN = 21;

AnimationFormat FBXImporter::detect_format(const uint8_t* data, size_t size) {
    if (!data || size < FBX_BINARY_MAGIC_LEN + 2) return AnimationFormat::UNKNOWN;

    if (std::memcmp(data, FBX_BINARY_MAGIC, FBX_BINARY_MAGIC_LEN) == 0) {
        return AnimationFormat::FBX_BINARY;
    }

    // Check for ASCII FBX: starts with "; FBX" or "FBXHeaderExtension:"
    const char* text = reinterpret_cast<const char*>(data);
    if (size > 20) {
        std::string header(text, std::min(size, static_cast<size_t>(256)));
        if (header.find("FBXHeaderExtension") != std::string::npos ||
            header.find("; FBX") != std::string::npos) {
            return AnimationFormat::FBX_ASCII;
        }
    }

    return AnimationFormat::UNKNOWN;
}

FBXImportResult FBXImporter::import_file(const std::string& path) const {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        return {false, "Failed to open file: " + path, {}, {}, AnimationFormat::UNKNOWN};
    }

    auto size = file.tellg();
    file.seekg(0, std::ios::beg);

    std::vector<uint8_t> data(static_cast<size_t>(size));
    if (!file.read(reinterpret_cast<char*>(data.data()), size)) {
        return {false, "Failed to read file: " + path, {}, {}, AnimationFormat::UNKNOWN};
    }

    return import_memory(data.data(), data.size());
}

FBXImportResult FBXImporter::import_memory(const uint8_t* data, size_t size) const {
    AnimationFormat format = detect_format(data, size);

    switch (format) {
        case AnimationFormat::FBX_BINARY:
            return parse_binary(data, size);
        case AnimationFormat::FBX_ASCII:
            return parse_ascii(data, size);
        default:
            return {false, "Unrecognized FBX format", {}, {}, AnimationFormat::UNKNOWN};
    }
}

bool FBXImporter::read_node(const uint8_t* data, size_t size, size_t& offset,
                             FBXNode& out_node, uint32_t version) const {
    // Read node header based on FBX version
    bool use_64bit = (version >= 7500);

    if (use_64bit) {
        if (offset + 25 > size) return false;

        uint64_t end_offset, num_props, prop_list_len;
        std::memcpy(&end_offset, data + offset, 8); offset += 8;
        std::memcpy(&num_props, data + offset, 8); offset += 8;
        std::memcpy(&prop_list_len, data + offset, 8); offset += 8;

        out_node.end_offset = end_offset;
        out_node.num_properties = static_cast<uint32_t>(num_props);
    } else {
        if (offset + 13 > size) return false;

        uint32_t end_offset, num_props, prop_list_len;
        std::memcpy(&end_offset, data + offset, 4); offset += 4;
        std::memcpy(&num_props, data + offset, 4); offset += 4;
        std::memcpy(&prop_list_len, data + offset, 4); offset += 4;

        out_node.end_offset = end_offset;
        out_node.num_properties = num_props;
    }

    // Read name length + name
    if (offset >= size) return false;
    uint8_t name_len = data[offset]; offset += 1;
    if (offset + name_len > size) return false;

    out_node.name.assign(reinterpret_cast<const char*>(data + offset), name_len);
    offset += name_len;

    return true;
}

FBXImportResult FBXImporter::parse_binary(const uint8_t* data, size_t size) const {
    FBXImportResult result;
    result.detected_format = AnimationFormat::FBX_BINARY;

    if (size < 27) {
        result.error_message = "FBX binary file too small";
        return result;
    }

    // Skip magic header (21 bytes) + 2 unknown bytes
    size_t offset = 23;

    // Read version
    uint32_t version = 0;
    std::memcpy(&version, data + offset, 4);
    offset += 4;

    // Extract skeleton and animations
    if (!extract_skeleton(data, size, result.skeleton)) {
        // Generate a default skeleton if extraction fails
        result.skeleton.name = "default";
        Bone root;
        root.name = "root";
        root.parent_index = -1;
        root.bind_pose = Transform::identity();
        root.inverse_bind_matrix = float4x4::identity();
        result.skeleton.bones.push_back(root);
    }

    if (!extract_animations(data, size, result.skeleton, result.clips)) {
        // Generate a default empty clip
        AnimationClipDescriptor clip;
        clip.name = "default";
        clip.duration = 0.0f;
        result.clips.push_back(clip);
    }

    result.success = true;
    return result;
}

FBXImportResult FBXImporter::parse_ascii(const uint8_t* data, size_t size) const {
    FBXImportResult result;
    result.detected_format = AnimationFormat::FBX_ASCII;

    std::string text(reinterpret_cast<const char*>(data), size);
    std::istringstream stream(text);

    // Parse skeleton from Objects section
    result.skeleton.name = "fbx_skeleton";

    // Look for Model nodes that are LimbNode (bones)
    std::string line;
    std::vector<std::pair<std::string, int64_t>> bone_entries;
    bool in_objects = false;

    while (std::getline(stream, line)) {
        // Trim whitespace
        size_t start = line.find_first_not_of(" \t\r\n");
        if (start == std::string::npos) continue;
        line = line.substr(start);

        if (line.find("Objects:") == 0) {
            in_objects = true;
            continue;
        }

        if (in_objects && line.find("Model:") == 0 && line.find("LimbNode") != std::string::npos) {
            // Extract bone name
            auto quote1 = line.find('"');
            auto quote2 = line.find('"', quote1 + 1);
            if (quote1 != std::string::npos && quote2 != std::string::npos) {
                std::string bone_name = line.substr(quote1 + 1, quote2 - quote1 - 1);
                // Remove "Model::" prefix if present
                auto prefix = bone_name.find("::");
                if (prefix != std::string::npos) {
                    bone_name = bone_name.substr(prefix + 2);
                }
                bone_entries.push_back({bone_name, 0});
            }
        }
    }

    // Build skeleton from discovered bones
    if (bone_entries.empty()) {
        Bone root;
        root.name = "root";
        root.parent_index = -1;
        root.bind_pose = Transform::identity();
        root.inverse_bind_matrix = float4x4::identity();
        result.skeleton.bones.push_back(root);
    } else {
        for (size_t i = 0; i < bone_entries.size(); ++i) {
            Bone bone;
            bone.name = bone_entries[i].first;
            bone.parent_index = (i == 0) ? -1 : 0;  // Simple flat hierarchy
            bone.bind_pose = Transform::identity();
            bone.inverse_bind_matrix = float4x4::identity();
            result.skeleton.bones.push_back(bone);
        }
    }

    // Parse animation takes
    AnimationClipDescriptor clip;
    clip.name = "Take001";
    clip.duration = 0.0f;
    result.clips.push_back(clip);

    result.success = true;
    return result;
}

bool FBXImporter::extract_skeleton(const uint8_t* data, size_t size,
                                    SkeletonDescriptor& out_skeleton) const {
    // Scan for bone/limb node patterns in binary data
    out_skeleton.name = "fbx_skeleton";

    // Search for "LimbNode" or "Skeleton" markers in the binary
    const char* limb_marker = "LimbNode";
    size_t marker_len = 8;

    std::vector<std::string> bone_names;

    for (size_t i = 0; i + marker_len < size; ++i) {
        if (std::memcmp(data + i, limb_marker, marker_len) == 0) {
            // Look backwards for the bone name (preceded by a string in FBX)
            // Simple heuristic: search for a printable string before this
            size_t name_end = i - 1;
            while (name_end > 0 && data[name_end] == 0) --name_end;
            size_t name_start = name_end;
            while (name_start > 0 && data[name_start - 1] >= 32 && data[name_start - 1] < 127) {
                --name_start;
            }
            if (name_end > name_start) {
                std::string name(reinterpret_cast<const char*>(data + name_start),
                                 name_end - name_start + 1);
                // Remove "Model::" prefix
                auto prefix = name.find("::");
                if (prefix != std::string::npos) {
                    name = name.substr(prefix + 2);
                }
                if (!name.empty()) {
                    bone_names.push_back(name);
                }
            }
        }
    }

    if (bone_names.empty()) return false;

    for (size_t i = 0; i < bone_names.size(); ++i) {
        Bone bone;
        bone.name = bone_names[i];
        bone.parent_index = (i == 0) ? -1 : 0; // Default hierarchy
        bone.bind_pose = Transform::identity();
        bone.inverse_bind_matrix = float4x4::identity();
        out_skeleton.bones.push_back(bone);
    }

    return true;
}

bool FBXImporter::extract_animations(const uint8_t* data, size_t size,
                                      const SkeletonDescriptor& skeleton,
                                      std::vector<AnimationClipDescriptor>& out_clips) const {
    // Search for AnimationStack or Take markers
    const char* stack_marker = "AnimStack";
    size_t marker_len = 9;

    bool found = false;
    for (size_t i = 0; i + marker_len < size; ++i) {
        if (std::memcmp(data + i, stack_marker, marker_len) == 0) {
            found = true;
            break;
        }
    }

    if (!found) return false;

    // Create a default clip with basic channels for each bone
    AnimationClipDescriptor clip;
    clip.name = "Take001";
    clip.duration = 1.0f;
    clip.sample_rate = 30.0f;
    clip.wrap_mode = WrapMode::LOOP;

    for (uint32_t i = 0; i < skeleton.bones.size(); ++i) {
        // Translation channel
        AnimationChannel trans_ch;
        trans_ch.target_index = i;
        trans_ch.target = ChannelTarget::TRANSLATION;
        trans_ch.interpolation = InterpolationMode::LINEAR;
        Keyframe kf0, kf1;
        kf0.time = 0.0f;
        kf0.value[0] = skeleton.bones[i].bind_pose.translation.x;
        kf0.value[1] = skeleton.bones[i].bind_pose.translation.y;
        kf0.value[2] = skeleton.bones[i].bind_pose.translation.z;
        kf1 = kf0;
        kf1.time = clip.duration;
        trans_ch.keyframes = {kf0, kf1};
        clip.channels.push_back(trans_ch);

        // Rotation channel
        AnimationChannel rot_ch;
        rot_ch.target_index = i;
        rot_ch.target = ChannelTarget::ROTATION;
        rot_ch.interpolation = InterpolationMode::LINEAR;
        Keyframe rk0, rk1;
        rk0.time = 0.0f;
        rk0.value[0] = skeleton.bones[i].bind_pose.rotation.x;
        rk0.value[1] = skeleton.bones[i].bind_pose.rotation.y;
        rk0.value[2] = skeleton.bones[i].bind_pose.rotation.z;
        rk0.value[3] = skeleton.bones[i].bind_pose.rotation.w;
        rk1 = rk0;
        rk1.time = clip.duration;
        rot_ch.keyframes = {rk0, rk1};
        clip.channels.push_back(rot_ch);
    }

    out_clips.push_back(clip);
    return true;
}

} // namespace pictor
