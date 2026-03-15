#include "pictor/animation/bvh_importer.h"
#include <fstream>
#include <sstream>
#include <cstring>
#include <cmath>
#include <algorithm>

namespace pictor {

static const float DEG_TO_RAD = 3.14159265358979f / 180.0f;

/// Skip whitespace in text
static void skip_ws(const char*& c, const char* end) {
    while (c < end && (*c == ' ' || *c == '\t' || *c == '\r' || *c == '\n')) ++c;
}

/// Read a token (non-whitespace string)
static std::string read_token(const char*& c, const char* end) {
    skip_ws(c, end);
    const char* start = c;
    while (c < end && *c != ' ' && *c != '\t' && *c != '\r' && *c != '\n') ++c;
    return std::string(start, c);
}

/// Read a float
static float read_float(const char*& c, const char* end) {
    std::string token = read_token(c, end);
    try { return std::stof(token); }
    catch (...) { return 0.0f; }
}

/// Read an integer
static int read_int(const char*& c, const char* end) {
    std::string token = read_token(c, end);
    try { return std::stoi(token); }
    catch (...) { return 0; }
}

BVHImportResult BVHImporter::import_file(const std::string& path) const {
    std::ifstream file(path);
    if (!file.is_open()) {
        return {false, "Failed to open BVH file: " + path, {}, {}, 0, 0.0f};
    }

    std::string content((std::istreambuf_iterator<char>(file)),
                         std::istreambuf_iterator<char>());
    return import_memory(content.data(), content.size());
}

BVHImportResult BVHImporter::import_memory(const char* data, size_t size) const {
    BVHImportResult result;

    if (!data || size == 0) {
        result.error_message = "Empty BVH data";
        return result;
    }

    const char* cursor = data;
    const char* end = data + size;

    // Parse HIERARCHY
    std::string token = read_token(cursor, end);
    if (token != "HIERARCHY") {
        result.error_message = "Expected HIERARCHY, got: " + token;
        return result;
    }

    std::vector<JointInfo> joints;
    if (!parse_hierarchy(cursor, end, joints)) {
        result.error_message = "Failed to parse HIERARCHY section";
        return result;
    }

    // Parse MOTION
    token = read_token(cursor, end);
    if (token != "MOTION") {
        result.error_message = "Expected MOTION, got: " + token;
        return result;
    }

    uint32_t frame_count = 0;
    float frame_time = 0.0f;
    std::vector<std::vector<float>> frame_data;

    if (!parse_motion(cursor, end, joints, frame_count, frame_time, frame_data)) {
        result.error_message = "Failed to parse MOTION section";
        return result;
    }

    result.skeleton = build_skeleton(joints);
    result.clip = build_clip(joints, frame_data, frame_count, frame_time);
    result.frame_count = frame_count;
    result.frame_time = frame_time;
    result.success = true;

    return result;
}

bool BVHImporter::parse_hierarchy(const char*& cursor, const char* end,
                                   std::vector<JointInfo>& joints) const {
    std::string token = read_token(cursor, end);
    if (token != "ROOT") return false;

    return parse_joint(cursor, end, joints, -1);
}

bool BVHImporter::parse_joint(const char*& cursor, const char* end,
                               std::vector<JointInfo>& joints,
                               int32_t parent_index) const {
    JointInfo joint;
    joint.name = read_token(cursor, end);
    joint.parent_index = parent_index;
    joint.is_end_site = false;

    // Compute channel start index
    uint32_t total_channels = 0;
    for (const auto& j : joints) {
        total_channels += static_cast<uint32_t>(j.channel_order.size());
    }
    joint.channel_start = total_channels;

    // Expect opening brace
    std::string token = read_token(cursor, end);
    if (token != "{") return false;

    int32_t current_index = static_cast<int32_t>(joints.size());
    joints.push_back(joint);

    while (cursor < end) {
        token = read_token(cursor, end);

        if (token == "}") {
            return true;
        } else if (token == "OFFSET") {
            joints[current_index].offset.x = read_float(cursor, end) * config_.scale;
            joints[current_index].offset.y = read_float(cursor, end) * config_.scale;
            joints[current_index].offset.z = read_float(cursor, end) * config_.scale;

            if (config_.z_up_to_y_up) {
                float temp = joints[current_index].offset.y;
                joints[current_index].offset.y = joints[current_index].offset.z;
                joints[current_index].offset.z = -temp;
            }
        } else if (token == "CHANNELS") {
            int num_channels = read_int(cursor, end);
            for (int i = 0; i < num_channels; ++i) {
                joints[current_index].channel_order.push_back(read_token(cursor, end));
            }
        } else if (token == "JOINT") {
            if (!parse_joint(cursor, end, joints, current_index)) return false;
        } else if (token == "End") {
            // End Site
            token = read_token(cursor, end); // "Site"
            token = read_token(cursor, end); // "{"
            if (token != "{") return false;

            JointInfo end_site;
            end_site.name = joints[current_index].name + "_end";
            end_site.parent_index = current_index;
            end_site.is_end_site = true;

            token = read_token(cursor, end); // "OFFSET"
            end_site.offset.x = read_float(cursor, end) * config_.scale;
            end_site.offset.y = read_float(cursor, end) * config_.scale;
            end_site.offset.z = read_float(cursor, end) * config_.scale;

            if (config_.z_up_to_y_up) {
                float temp = end_site.offset.y;
                end_site.offset.y = end_site.offset.z;
                end_site.offset.z = -temp;
            }

            joints.push_back(end_site);

            token = read_token(cursor, end); // "}"
            if (token != "}") return false;
        }
    }

    return false; // Missing closing brace
}

bool BVHImporter::parse_motion(const char*& cursor, const char* end,
                                const std::vector<JointInfo>& joints,
                                uint32_t& frame_count,
                                float& frame_time,
                                std::vector<std::vector<float>>& frame_data) const {
    // "Frames:" line
    std::string token = read_token(cursor, end);
    if (token != "Frames:") return false;
    frame_count = static_cast<uint32_t>(read_int(cursor, end));

    // "Frame Time:" line
    token = read_token(cursor, end);
    if (token != "Frame") return false;
    token = read_token(cursor, end); // "Time:"
    frame_time = read_float(cursor, end);

    // Count total channels
    uint32_t total_channels = 0;
    for (const auto& j : joints) {
        total_channels += static_cast<uint32_t>(j.channel_order.size());
    }

    // Read frame data
    frame_data.resize(frame_count);
    for (uint32_t f = 0; f < frame_count; ++f) {
        frame_data[f].resize(total_channels);
        for (uint32_t c = 0; c < total_channels; ++c) {
            frame_data[f][c] = read_float(cursor, end);
        }
    }

    return true;
}

SkeletonDescriptor BVHImporter::build_skeleton(const std::vector<JointInfo>& joints) const {
    SkeletonDescriptor skeleton;
    skeleton.name = "bvh_skeleton";

    for (const auto& joint : joints) {
        Bone bone;
        bone.name = joint.name;
        bone.parent_index = joint.parent_index;
        bone.bind_pose.translation = joint.offset;
        bone.bind_pose.rotation = Quaternion::identity();
        bone.bind_pose.scale = {1.0f, 1.0f, 1.0f};
        bone.inverse_bind_matrix = float4x4::identity();
        skeleton.bones.push_back(bone);
    }

    return skeleton;
}

AnimationClipDescriptor BVHImporter::build_clip(const std::vector<JointInfo>& joints,
                                                 const std::vector<std::vector<float>>& frame_data,
                                                 uint32_t frame_count,
                                                 float frame_time) const {
    AnimationClipDescriptor clip;
    clip.name = "bvh_motion";
    clip.duration = static_cast<float>(frame_count) * frame_time;
    clip.sample_rate = (frame_time > 0.0f) ? 1.0f / frame_time : 30.0f;
    clip.wrap_mode = WrapMode::LOOP;

    for (uint32_t j = 0; j < joints.size(); ++j) {
        const auto& joint = joints[j];
        if (joint.is_end_site || joint.channel_order.empty()) continue;

        // Create translation and rotation channels for this joint
        bool has_position = false;
        bool has_rotation = false;

        // Check which channels this joint has
        int pos_channels[3] = {-1, -1, -1}; // X, Y, Z position channel indices
        int rot_channels[3] = {-1, -1, -1}; // rotation channel indices in order
        std::string rot_order;

        for (uint32_t c = 0; c < joint.channel_order.size(); ++c) {
            const auto& ch = joint.channel_order[c];
            uint32_t global_idx = joint.channel_start + c;

            if (ch == "Xposition") { pos_channels[0] = global_idx; has_position = true; }
            else if (ch == "Yposition") { pos_channels[1] = global_idx; has_position = true; }
            else if (ch == "Zposition") { pos_channels[2] = global_idx; has_position = true; }
            else if (ch.size() >= 9 && ch.substr(1) == "rotation") {
                has_rotation = true;
                rot_order += ch[0]; // 'X', 'Y', or 'Z'
                int idx = static_cast<int>(rot_order.size()) - 1;
                if (idx < 3) rot_channels[idx] = global_idx;
            }
        }

        // Build position channel
        if (has_position) {
            AnimationChannel trans_ch;
            trans_ch.target_index = j;
            trans_ch.target = ChannelTarget::TRANSLATION;
            trans_ch.interpolation = InterpolationMode::LINEAR;

            for (uint32_t f = 0; f < frame_count; ++f) {
                Keyframe kf;
                kf.time = static_cast<float>(f) * frame_time;
                kf.value[0] = (pos_channels[0] >= 0) ? frame_data[f][pos_channels[0]] * config_.scale : joint.offset.x;
                kf.value[1] = (pos_channels[1] >= 0) ? frame_data[f][pos_channels[1]] * config_.scale : joint.offset.y;
                kf.value[2] = (pos_channels[2] >= 0) ? frame_data[f][pos_channels[2]] * config_.scale : joint.offset.z;

                if (config_.z_up_to_y_up) {
                    float temp = kf.value[1];
                    kf.value[1] = kf.value[2];
                    kf.value[2] = -temp;
                }

                trans_ch.keyframes.push_back(kf);
            }

            clip.channels.push_back(trans_ch);
        }

        // Build rotation channel
        if (has_rotation) {
            AnimationChannel rot_ch;
            rot_ch.target_index = j;
            rot_ch.target = ChannelTarget::ROTATION;
            rot_ch.interpolation = InterpolationMode::LINEAR;

            for (uint32_t f = 0; f < frame_count; ++f) {
                float angles[3] = {0.0f, 0.0f, 0.0f};
                for (int a = 0; a < 3; ++a) {
                    if (rot_channels[a] >= 0) {
                        angles[a] = frame_data[f][rot_channels[a]];
                        if (config_.degrees_to_rad) {
                            angles[a] *= DEG_TO_RAD;
                        }
                    }
                }

                // Convert Euler angles to quaternion based on rotation order
                Quaternion q = Quaternion::identity();
                for (size_t r = 0; r < rot_order.size() && r < 3; ++r) {
                    switch (rot_order[r]) {
                        case 'X': q = q * Quaternion::from_axis_angle({1, 0, 0}, angles[r]); break;
                        case 'Y': q = q * Quaternion::from_axis_angle({0, 1, 0}, angles[r]); break;
                        case 'Z': q = q * Quaternion::from_axis_angle({0, 0, 1}, angles[r]); break;
                    }
                }

                Keyframe kf;
                kf.time = static_cast<float>(f) * frame_time;
                kf.value[0] = q.x;
                kf.value[1] = q.y;
                kf.value[2] = q.z;
                kf.value[3] = q.w;
                rot_ch.keyframes.push_back(kf);
            }

            clip.channels.push_back(rot_ch);
        }
    }

    return clip;
}

} // namespace pictor
