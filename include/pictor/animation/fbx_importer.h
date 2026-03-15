#pragma once

#include "pictor/animation/animation_types.h"
#include "pictor/animation/animation_clip.h"
#include "pictor/animation/skeleton.h"
#include <string>
#include <vector>

namespace pictor {

/// Result of an FBX animation import
struct FBXImportResult {
    bool                          success = false;
    std::string                   error_message;
    SkeletonDescriptor            skeleton;
    std::vector<AnimationClipDescriptor> clips;
    AnimationFormat               detected_format = AnimationFormat::UNKNOWN;
};

/// FBX animation file importer.
/// Supports both binary FBX and ASCII FBX formats.
/// Extracts skeleton hierarchy, bind poses, and animation clips.
class FBXImporter {
public:
    FBXImporter() = default;
    ~FBXImporter() = default;

    /// Import from file path
    FBXImportResult import_file(const std::string& path) const;

    /// Import from memory buffer
    FBXImportResult import_memory(const uint8_t* data, size_t size) const;

    /// Detect whether the data is binary or ASCII FBX
    static AnimationFormat detect_format(const uint8_t* data, size_t size);

private:
    /// Parse binary FBX format
    FBXImportResult parse_binary(const uint8_t* data, size_t size) const;

    /// Parse ASCII FBX format
    FBXImportResult parse_ascii(const uint8_t* data, size_t size) const;

    /// Extract skeleton from FBX node hierarchy
    bool extract_skeleton(const uint8_t* data, size_t size,
                          SkeletonDescriptor& out_skeleton) const;

    /// Extract animation curves from FBX takes
    bool extract_animations(const uint8_t* data, size_t size,
                            const SkeletonDescriptor& skeleton,
                            std::vector<AnimationClipDescriptor>& out_clips) const;

    /// Parse FBX node (binary format)
    struct FBXNode {
        std::string name;
        uint64_t    end_offset   = 0;
        uint32_t    num_properties = 0;
        std::vector<FBXNode> children;
    };

    /// Read a binary FBX node
    bool read_node(const uint8_t* data, size_t size, size_t& offset,
                   FBXNode& out_node, uint32_t version) const;
};

} // namespace pictor
