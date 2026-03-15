#pragma once

#include "pictor/animation/animation_types.h"
#include "pictor/animation/animation_clip.h"
#include "pictor/animation/skeleton.h"
#include <string>
#include <vector>

namespace pictor {

/// Result of a BVH animation import
struct BVHImportResult {
    bool                    success = false;
    std::string             error_message;
    SkeletonDescriptor      skeleton;
    AnimationClipDescriptor clip;
    uint32_t                frame_count = 0;
    float                   frame_time  = 0.0f;
};

/// BVH (BioVision Hierarchy) motion capture file importer.
/// Parses the HIERARCHY and MOTION sections to extract skeleton and animation data.
class BVHImporter {
public:
    BVHImporter() = default;
    ~BVHImporter() = default;

    /// Import from file path
    BVHImportResult import_file(const std::string& path) const;

    /// Import from memory buffer (text data)
    BVHImportResult import_memory(const char* data, size_t size) const;

    /// Configuration for BVH import
    struct Config {
        float scale          = 1.0f;   // Scale factor for positions
        bool  z_up_to_y_up   = false;  // Convert Z-up to Y-up coordinate system
        bool  degrees_to_rad = true;   // Convert degrees to radians
    };

    void set_config(const Config& config) { config_ = config; }
    const Config& config() const { return config_; }

private:
    /// BVH joint info during parsing
    struct JointInfo {
        std::string name;
        int32_t     parent_index = -1;
        float3      offset;
        std::vector<std::string> channel_order; // e.g., "Xposition", "Yrotation"
        uint32_t    channel_start = 0;
        bool        is_end_site = false;
    };

    /// Parse HIERARCHY section
    bool parse_hierarchy(const char*& cursor, const char* end,
                         std::vector<JointInfo>& joints) const;

    /// Parse a single joint/end-site block
    bool parse_joint(const char*& cursor, const char* end,
                     std::vector<JointInfo>& joints,
                     int32_t parent_index) const;

    /// Parse MOTION section
    bool parse_motion(const char*& cursor, const char* end,
                      const std::vector<JointInfo>& joints,
                      uint32_t& frame_count,
                      float& frame_time,
                      std::vector<std::vector<float>>& frame_data) const;

    /// Convert parsed data to skeleton descriptor
    SkeletonDescriptor build_skeleton(const std::vector<JointInfo>& joints) const;

    /// Convert parsed data to animation clip descriptor
    AnimationClipDescriptor build_clip(const std::vector<JointInfo>& joints,
                                       const std::vector<std::vector<float>>& frame_data,
                                       uint32_t frame_count,
                                       float frame_time) const;

    Config config_;
};

} // namespace pictor
