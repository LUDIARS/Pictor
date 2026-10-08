#include "track_csv.h"

#include "pictor/core/float_parse.h"

#include <cmath>
#include <cstdio>
#include <unordered_map>

namespace pictor_fbx_viewer {

namespace {

constexpr std::string_view kBonePrefix  = "bone:";
constexpr std::string_view kMorphPrefix = "morph:";
constexpr uint32_t kIgnored = UINT32_MAX;

enum class ColumnKind : uint8_t { Ignored, BoneAxis, Morph };

struct ColumnTarget {
    ColumnKind kind  = ColumnKind::Ignored;
    uint32_t   slot  = 0;  // bone group or morph channel
    uint32_t   axis  = 0;  // 0..2 for BoneAxis
};

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
    return s;
}

/// Split one line on ',' into `cells` (views into `line`).
void split_cells(std::string_view line, std::vector<std::string_view>& cells) {
    cells.clear();
    size_t start = 0;
    for (;;) {
        const size_t comma = line.find(',', start);
        if (comma == std::string_view::npos) {
            cells.push_back(trim(line.substr(start)));
            return;
        }
        cells.push_back(trim(line.substr(start, comma - start)));
        start = comma + 1;
    }
}

bool parse_number(std::string_view cell, double& value) {
    if (cell.empty()) return false;
    const char* first = cell.data();
    const char* last  = first + cell.size();
    const auto r = pictor::float_parse::parse_double(first, last, value);
    return r.ec == std::errc() && r.ptr == last && std::isfinite(value);
}

uint32_t find_name(const std::unordered_map<std::string_view, uint32_t>& map, std::string_view name) {
    const auto it = map.find(name);
    return it == map.end() ? kIgnored : it->second;
}

std::string at_row(size_t row) { return " (row " + std::to_string(row + 1) + ")"; }

} // namespace

/// @implements SPEC-PC-FBX-TRACK-PLAYBACK
bool parse_track_csv(std::string_view text,
                     const std::vector<std::string>& bone_names,
                     const std::vector<std::string>& shape_names,
                     TrackData& out, std::string& error) {
    out = TrackData{};
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
        static_cast<unsigned char>(text[1]) == 0xBB && static_cast<unsigned char>(text[2]) == 0xBF) {
        text.remove_prefix(3);
    }

    // Collect non-empty lines (trailing blank lines are tolerated).
    std::vector<std::string_view> lines;
    for (size_t pos = 0; pos <= text.size();) {
        size_t nl = text.find('\n', pos);
        if (nl == std::string_view::npos) nl = text.size();
        const std::string_view line = trim(text.substr(pos, nl - pos));
        if (!line.empty()) lines.push_back(line);
        pos = nl + 1;
    }
    if (lines.empty()) { error = "track file is empty"; return false; }

    std::unordered_map<std::string_view, uint32_t> bone_lookup, shape_lookup;
    for (size_t i = 0; i < bone_names.size(); ++i)
        bone_lookup.emplace(bone_names[i], static_cast<uint32_t>(i));
    for (size_t i = 0; i < shape_names.size(); ++i)
        shape_lookup.emplace(shape_names[i], static_cast<uint32_t>(i));

    // ── Header → column targets ──────────────────────────────
    std::vector<std::string_view> cells;
    split_cells(lines[0], cells);
    if (cells.empty() || cells[0] != "frame") {
        error = "first header column must be 'frame'";
        return false;
    }
    std::vector<ColumnTarget> columns(cells.size());
    std::unordered_map<uint32_t, uint32_t> group_of_bone, channel_of_shape;
    std::vector<uint8_t> axis_seen;  // [group][axis]
    std::unordered_map<std::string_view, bool> warned;
    auto warn_unknown = [&](std::string_view kind, std::string_view name) {
        if (warned.emplace(name, true).second)
            out.warnings.push_back("unknown " + std::string(kind) + " '" + std::string(name) + "' ignored");
    };

    for (size_t c = 1; c < cells.size(); ++c) {
        const std::string_view h = cells[c];
        if (h.substr(0, kBonePrefix.size()) == kBonePrefix) {
            const std::string_view rest = h.substr(kBonePrefix.size());
            const size_t dot = rest.rfind('.');
            const std::string_view axis_name = dot == std::string_view::npos ? std::string_view{} : rest.substr(dot + 1);
            uint32_t axis = 0;
            if (axis_name == "rx") axis = 0;
            else if (axis_name == "ry") axis = 1;
            else if (axis_name == "rz") axis = 2;
            else { error = "bone column needs a .rx/.ry/.rz suffix: " + std::string(h); return false; }
            const std::string_view bone = rest.substr(0, dot);
            const uint32_t bone_index = find_name(bone_lookup, bone);
            if (bone_index == kIgnored) { warn_unknown("bone", bone); continue; }
            auto [it, added] = group_of_bone.emplace(bone_index, out.bone_group_count());
            if (added) {
                out.bone_index.push_back(bone_index);
                axis_seen.resize(axis_seen.size() + 3, 0);
            }
            if (axis_seen[it->second * 3 + axis]) { error = "duplicate column: " + std::string(h); return false; }
            axis_seen[it->second * 3 + axis] = 1;
            columns[c] = {ColumnKind::BoneAxis, it->second, axis};
        } else if (h.substr(0, kMorphPrefix.size()) == kMorphPrefix) {
            const std::string_view shape = h.substr(kMorphPrefix.size());
            const uint32_t shape_index = find_name(shape_lookup, shape);
            if (shape_index == kIgnored) { warn_unknown("shape", shape); continue; }
            auto [it, added] = channel_of_shape.emplace(shape_index, out.morph_count());
            if (!added) { error = "duplicate column: " + std::string(h); return false; }
            out.morph_index.push_back(shape_index);
            columns[c] = {ColumnKind::Morph, it->second, 0};
        } else {
            error = "unknown column (expected bone:<name>.rx|ry|rz or morph:<name>): " + std::string(h);
            return false;
        }
    }
    if (out.bone_index.empty() && out.morph_index.empty()) {
        error = "track has no column that matches a bone or shape of the model";
        return false;
    }

    // ── Rows → flat frame-major arrays ───────────────────────
    out.frame_count = static_cast<uint32_t>(lines.size() - 1);
    const size_t groups = out.bone_index.size();
    const size_t morphs = out.morph_index.size();
    out.bone_euler_deg.assign(static_cast<size_t>(out.frame_count) * groups * 3, 0.0f);
    out.morph_weight.assign(static_cast<size_t>(out.frame_count) * morphs, 0.0f);

    for (size_t r = 1; r < lines.size(); ++r) {
        const uint32_t frame = static_cast<uint32_t>(r - 1);
        split_cells(lines[r], cells);
        if (cells.size() != columns.size()) {
            error = "expected " + std::to_string(columns.size()) + " fields, got " +
                    std::to_string(cells.size()) + at_row(r);
            return false;
        }
        double v = 0.0;
        if (!parse_number(cells[0], v) || v != static_cast<double>(frame)) {
            error = "frame must be " + std::to_string(frame) + " (no gaps)" + at_row(r);
            return false;
        }
        for (size_t c = 1; c < cells.size(); ++c) {
            if (!parse_number(cells[c], v)) {
                error = "non-numeric value '" + std::string(cells[c]) + "'" + at_row(r);
                return false;
            }
            const ColumnTarget& t = columns[c];
            if (t.kind == ColumnKind::BoneAxis)
                out.bone_euler_deg[(frame * groups + t.slot) * 3 + t.axis] = static_cast<float>(v);
            else if (t.kind == ColumnKind::Morph)
                out.morph_weight[frame * morphs + t.slot] = static_cast<float>(v);
        }
    }
    if (out.frame_count == 0) { error = "track has a header but no frames"; return false; }
    return true;
}

/// @implements SPEC-PC-FBX-TRACK-PLAYBACK
bool load_track_csv(const std::string& path,
                    const std::vector<std::string>& bone_names,
                    const std::vector<std::string>& shape_names,
                    TrackData& out, std::string& error) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) { error = "cannot open " + path; return false; }
    std::string text;
    char buf[1 << 16];
    size_t n = 0;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, n);
    const bool read_ok = std::ferror(f) == 0;
    std::fclose(f);
    if (!read_ok) { error = "cannot read " + path; return false; }
    return parse_track_csv(text, bone_names, shape_names, out, error);
}

} // namespace pictor_fbx_viewer
