#include "pictor/text/font_loader.h"
#include <fstream>
#include <cstring>
#include <algorithm>
#include <cmath>

namespace pictor {

// ============================================================
// Big-endian read helpers (font files are always big-endian)
// ============================================================

namespace {

inline uint16_t read_u16(const uint8_t* p) {
    return static_cast<uint16_t>((p[0] << 8) | p[1]);
}

inline int16_t read_i16(const uint8_t* p) {
    return static_cast<int16_t>(read_u16(p));
}

inline uint32_t read_u32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) <<  8) |
            static_cast<uint32_t>(p[3]);
}

inline uint32_t make_tag(char a, char b, char c, char d) {
    return (static_cast<uint32_t>(static_cast<uint8_t>(a)) << 24) |
           (static_cast<uint32_t>(static_cast<uint8_t>(b)) << 16) |
           (static_cast<uint32_t>(static_cast<uint8_t>(c)) <<  8) |
            static_cast<uint32_t>(static_cast<uint8_t>(d));
}

} // anonymous namespace

// ============================================================
// Loading
// ============================================================

FontHandle FontLoader::load_from_file(const std::string& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) return INVALID_FONT;

    auto file_size = file.tellg();
    if (file_size <= 0) return INVALID_FONT;

    std::vector<uint8_t> buffer(static_cast<size_t>(file_size));
    file.seekg(0);
    file.read(reinterpret_cast<char*>(buffer.data()), file_size);
    if (!file) return INVALID_FONT;

    // Extract family name from file path
    std::string name = path;
    auto slash = name.find_last_of("/\\");
    if (slash != std::string::npos) name = name.substr(slash + 1);
    auto dot = name.rfind('.');
    if (dot != std::string::npos) name = name.substr(0, dot);

    return load_from_memory(buffer.data(), buffer.size(), name);
}

FontHandle FontLoader::load_from_memory(const void* data, size_t size,
                                         const std::string& name) {
    if (!data || size < 12) return INVALID_FONT;

    FontHandle handle = next_handle_++;

    FontTableEntry entry;
    entry.handle = handle;
    entry.family_name = name;
    entry.style_name = "Regular";
    entry.raw_data.assign(static_cast<const uint8_t*>(data),
                          static_cast<const uint8_t*>(data) + size);

    if (!parse_font_tables(entry)) {
        return INVALID_FONT;
    }

    entry.loaded = true;
    fonts_.push_back(std::move(entry));

    return handle;
}

void FontLoader::unload(FontHandle handle) {
    for (size_t i = 0; i < fonts_.size(); ++i) {
        if (fonts_[i].handle == handle) {
            fonts_.erase(fonts_.begin() + static_cast<ptrdiff_t>(i));
            parsed_.erase(parsed_.begin() + static_cast<ptrdiff_t>(i));
            return;
        }
    }
}

// ============================================================
// Query
// ============================================================

bool FontLoader::is_valid(FontHandle handle) const {
    for (const auto& f : fonts_) {
        if (f.handle == handle) return f.loaded;
    }
    return false;
}

const FontMetrics* FontLoader::get_metrics(FontHandle handle) const {
    for (const auto& f : fonts_) {
        if (f.handle == handle) return &f.metrics;
    }
    return nullptr;
}

const std::string* FontLoader::get_family_name(FontHandle handle) const {
    for (const auto& f : fonts_) {
        if (f.handle == handle) return &f.family_name;
    }
    return nullptr;
}

bool FontLoader::get_glyph_metrics(FontHandle handle, uint32_t codepoint,
                                    float font_size, GlyphMetrics& out) const {
    size_t idx = 0;
    bool found = false;
    for (size_t i = 0; i < fonts_.size(); ++i) {
        if (fonts_[i].handle == handle) { idx = i; found = true; break; }
    }
    if (!found) return false;

    const auto& parsed = parsed_[idx];
    const auto& entry = fonts_[idx];

    auto it = parsed.cmap.find(codepoint);
    if (it == parsed.cmap.end()) return false;

    uint16_t glyph_index = it->second;
    if (glyph_index == 0) return false;

    float scale = (entry.metrics.units_per_em > 0)
                  ? font_size / static_cast<float>(entry.metrics.units_per_em)
                  : 1.0f;

    out.codepoint = codepoint;
    out.advance_x = (glyph_index < parsed.advance_widths.size())
                    ? parsed.advance_widths[glyph_index] * scale
                    : 0.0f;
    out.bearing_x = (glyph_index < parsed.left_side_bearings.size())
                    ? parsed.left_side_bearings[glyph_index] * scale
                    : 0.0f;
    out.bearing_y = entry.metrics.ascender * scale;
    out.width     = out.advance_x;
    out.height    = (entry.metrics.ascender - entry.metrics.descender) * scale;

    return true;
}

uint16_t FontLoader::codepoint_to_glyph_index(FontHandle handle,
                                                uint32_t codepoint) const {
    for (size_t i = 0; i < fonts_.size(); ++i) {
        if (fonts_[i].handle == handle) {
            auto it = parsed_[i].cmap.find(codepoint);
            return (it != parsed_[i].cmap.end()) ? it->second : 0;
        }
    }
    return 0;
}

bool FontLoader::has_glyph(FontHandle handle, uint32_t codepoint) const {
    return codepoint_to_glyph_index(handle, codepoint) != 0;
}

const FontTableEntry* FontLoader::get_entry(FontHandle handle) const {
    for (const auto& f : fonts_) {
        if (f.handle == handle) return &f;
    }
    return nullptr;
}

int16_t FontLoader::get_kerning(FontHandle handle, uint32_t left_codepoint,
                                 uint32_t right_codepoint) const {
    for (size_t i = 0; i < fonts_.size(); ++i) {
        if (fonts_[i].handle == handle) {
            uint16_t left_gi = codepoint_to_glyph_index(handle, left_codepoint);
            uint16_t right_gi = codepoint_to_glyph_index(handle, right_codepoint);
            uint64_t key = (static_cast<uint64_t>(left_gi) << 16) | right_gi;
            auto it = parsed_[i].kern_map.find(key);
            return (it != parsed_[i].kern_map.end()) ? it->second : 0;
        }
    }
    return 0;
}

std::vector<FontHandle> FontLoader::all_handles() const {
    std::vector<FontHandle> handles;
    handles.reserve(fonts_.size());
    for (const auto& f : fonts_) handles.push_back(f.handle);
    return handles;
}

// ============================================================
// Internal: Table directory parsing
// ============================================================

std::vector<FontLoader::TableRecord> FontLoader::read_table_directory(
    const uint8_t* data, size_t size) {

    std::vector<TableRecord> records;

    if (size < 12) return records;

    uint32_t sfVersion = read_u32(data);
    // Accept TrueType (0x00010000), OpenType ('OTTO'), TrueType Collection ('ttcf')
    (void)sfVersion;

    uint16_t num_tables = read_u16(data + 4);
    size_t dir_offset = 12; // After offset-table header

    for (uint16_t i = 0; i < num_tables; ++i) {
        size_t rec_offset = dir_offset + i * 16;
        if (rec_offset + 16 > size) break;

        TableRecord rec;
        rec.tag    = read_u32(data + rec_offset);
        rec.offset = read_u32(data + rec_offset + 8);
        rec.length = read_u32(data + rec_offset + 12);
        records.push_back(rec);
    }

    return records;
}

// ============================================================
// Internal: Font table parsing
// ============================================================

bool FontLoader::parse_font_tables(FontTableEntry& entry) {
    const uint8_t* data = entry.raw_data.data();
    size_t size = entry.raw_data.size();

    auto tables = read_table_directory(data, size);
    if (tables.empty()) return false;

    ParsedFontData parsed;

    // Find key tables
    const uint8_t* head_data = nullptr; uint32_t head_len = 0;
    const uint8_t* hhea_data = nullptr; uint32_t hhea_len = 0;
    const uint8_t* hmtx_data = nullptr; uint32_t hmtx_len = 0;
    const uint8_t* maxp_data = nullptr; uint32_t maxp_len = 0;
    const uint8_t* cmap_data = nullptr; uint32_t cmap_len = 0;
    const uint8_t* os2_data  = nullptr; uint32_t os2_len  = 0;
    const uint8_t* kern_data = nullptr; uint32_t kern_len = 0;
    const uint8_t* name_data = nullptr; uint32_t name_len = 0;

    for (const auto& rec : tables) {
        if (rec.offset + rec.length > size) continue;
        const uint8_t* tdata = data + rec.offset;

        if      (rec.tag == make_tag('h','e','a','d')) { head_data = tdata; head_len = rec.length; }
        else if (rec.tag == make_tag('h','h','e','a')) { hhea_data = tdata; hhea_len = rec.length; }
        else if (rec.tag == make_tag('h','m','t','x')) { hmtx_data = tdata; hmtx_len = rec.length; }
        else if (rec.tag == make_tag('m','a','x','p')) { maxp_data = tdata; maxp_len = rec.length; }
        else if (rec.tag == make_tag('c','m','a','p')) { cmap_data = tdata; cmap_len = rec.length; }
        else if (rec.tag == make_tag('O','S','/','2')) { os2_data  = tdata; os2_len  = rec.length; }
        else if (rec.tag == make_tag('k','e','r','n')) { kern_data = tdata; kern_len = rec.length; }
        else if (rec.tag == make_tag('n','a','m','e')) { name_data = tdata; name_len = rec.length; }
        else if (rec.tag == make_tag('g','l','y','f')) {
            parsed.glyf_offset = rec.offset; parsed.glyf_length = rec.length;
        }
        else if (rec.tag == make_tag('l','o','c','a')) {
            parsed.loca_offset = rec.offset; parsed.loca_length = rec.length;
        }
        else if (rec.tag == make_tag('C','F','F',' ') ||
                 rec.tag == make_tag('C','F','F','2')) {
            parsed.is_cff = true;
        }
    }

    // ---- 'head' table ----
    if (head_data && head_len >= 54) {
        entry.metrics.units_per_em = read_u16(head_data + 18);
        parsed.index_to_loc_format = read_i16(head_data + 50);
    } else {
        entry.metrics.units_per_em = 1000; // Default for CFF fonts
    }

    // ---- 'maxp' table ----
    if (maxp_data && maxp_len >= 6) {
        parsed.num_glyphs = read_u16(maxp_data + 4);
    }

    // ---- 'hhea' table (ascender, descender, line gap, num h-metrics) ----
    if (hhea_data && hhea_len >= 36) {
        entry.metrics.ascender   = static_cast<float>(read_i16(hhea_data + 4));
        entry.metrics.descender  = static_cast<float>(read_i16(hhea_data + 6));
        float line_gap           = static_cast<float>(read_i16(hhea_data + 8));
        entry.metrics.line_height = entry.metrics.ascender - entry.metrics.descender + line_gap;
        parsed.num_h_metrics     = read_u16(hhea_data + 34);
    }

    // ---- 'OS/2' table (overrides ascender/descender if sTypo* is available) ----
    if (os2_data && os2_len >= 72) {
        int16_t typo_ascender  = read_i16(os2_data + 68);
        int16_t typo_descender = read_i16(os2_data + 70);
        if (typo_ascender != 0) {
            entry.metrics.ascender  = static_cast<float>(typo_ascender);
            entry.metrics.descender = static_cast<float>(typo_descender);
        }
        if (os2_len >= 74) {
            float typo_line_gap = static_cast<float>(read_i16(os2_data + 72));
            entry.metrics.line_height = entry.metrics.ascender -
                                        entry.metrics.descender + typo_line_gap;
        }
    }

    // ---- 'hmtx' table ----
    if (hmtx_data && hmtx_len > 0 && parsed.num_h_metrics > 0) {
        parsed.advance_widths.resize(parsed.num_glyphs, 0);
        parsed.left_side_bearings.resize(parsed.num_glyphs, 0);

        // Long horizontal metrics: (advanceWidth, lsb) for each
        for (uint16_t i = 0; i < parsed.num_h_metrics && (i * 4 + 3) < hmtx_len; ++i) {
            parsed.advance_widths[i]     = read_u16(hmtx_data + i * 4);
            parsed.left_side_bearings[i] = read_i16(hmtx_data + i * 4 + 2);
        }
        // Remaining glyphs share the last advance width
        uint16_t last_advance = (parsed.num_h_metrics > 0)
                                ? parsed.advance_widths[parsed.num_h_metrics - 1] : 0;
        for (uint16_t i = parsed.num_h_metrics; i < parsed.num_glyphs; ++i) {
            parsed.advance_widths[i] = last_advance;
            uint32_t lsb_off = parsed.num_h_metrics * 4 +
                               (i - parsed.num_h_metrics) * 2;
            if (lsb_off + 1 < hmtx_len) {
                parsed.left_side_bearings[i] = read_i16(hmtx_data + lsb_off);
            }
        }

        entry.metrics.max_advance = 0.0f;
        for (auto aw : parsed.advance_widths) {
            if (aw > entry.metrics.max_advance)
                entry.metrics.max_advance = static_cast<float>(aw);
        }
    }

    // ---- 'cmap' table ----
    if (cmap_data && cmap_len > 0) {
        parse_cmap(cmap_data, cmap_len, entry);
        // Transfer built cmap to parsed data
        // (parse_cmap populates parsed_.back().cmap through the entry index)
    }

    // ---- 'kern' table (format 0) ----
    if (kern_data && kern_len >= 4) {
        uint16_t kern_version = read_u16(kern_data);
        if (kern_version == 0 && kern_len >= 8) {
            uint16_t num_subtables = read_u16(kern_data + 2);
            uint32_t sub_offset = 4;
            for (uint16_t st = 0; st < num_subtables; ++st) {
                if (sub_offset + 6 > kern_len) break;
                uint16_t sub_length  = read_u16(kern_data + sub_offset + 2);
                uint16_t sub_coverage = read_u16(kern_data + sub_offset + 4);
                uint8_t  sub_format = sub_coverage >> 8;
                if (sub_format == 0 && sub_offset + 14 <= kern_len) {
                    uint16_t num_pairs = read_u16(kern_data + sub_offset + 6);
                    uint32_t pair_off = sub_offset + 14;
                    for (uint16_t p = 0; p < num_pairs; ++p) {
                        if (pair_off + 6 > kern_len) break;
                        uint16_t left  = read_u16(kern_data + pair_off);
                        uint16_t right = read_u16(kern_data + pair_off + 2);
                        int16_t  value = read_i16(kern_data + pair_off + 4);
                        uint64_t key = (static_cast<uint64_t>(left) << 16) | right;
                        parsed.kern_map[key] = value;
                        pair_off += 6;
                    }
                }
                sub_offset += sub_length;
            }
        }
    }

    // ---- 'name' table (extract family name if available) ----
    if (name_data && name_len >= 6) {
        uint16_t name_count = read_u16(name_data + 2);
        uint16_t string_offset = read_u16(name_data + 4);
        for (uint16_t i = 0; i < name_count; ++i) {
            uint32_t rec_off = 6 + i * 12;
            if (rec_off + 12 > name_len) break;
            uint16_t platform_id = read_u16(name_data + rec_off);
            uint16_t name_id     = read_u16(name_data + rec_off + 6);
            uint16_t str_length  = read_u16(name_data + rec_off + 8);
            uint16_t str_off     = read_u16(name_data + rec_off + 10);
            uint32_t abs_off     = string_offset + str_off;
            if (abs_off + str_length > name_len) continue;

            // nameID 1 = Font Family, nameID 2 = Font Subfamily
            if (name_id == 1 && platform_id == 1 && str_length > 0) {
                // Platform 1 (Macintosh) = single-byte encoding
                entry.family_name.assign(
                    reinterpret_cast<const char*>(name_data + abs_off), str_length);
            } else if (name_id == 1 && platform_id == 3 && str_length >= 2) {
                // Platform 3 (Windows) = UTF-16BE
                std::string decoded;
                for (uint16_t j = 0; j + 1 < str_length; j += 2) {
                    uint16_t ch = read_u16(name_data + abs_off + j);
                    if (ch < 128) decoded += static_cast<char>(ch);
                }
                if (!decoded.empty()) entry.family_name = decoded;
            }
            if (name_id == 2 && platform_id == 1 && str_length > 0) {
                entry.style_name.assign(
                    reinterpret_cast<const char*>(name_data + abs_off), str_length);
            }
        }
    }

    // Build cmap into parsed data
    // We need to do this before pushing parsed
    // parse_cmap already stored cmap in a temporary; move it
    // Actually parse_cmap needs parsed_ context. Let's build cmap here directly.

    // Re-parse cmap into parsed.cmap
    if (cmap_data && cmap_len >= 4) {
        uint16_t num_subtables = read_u16(cmap_data + 2);
        uint32_t best_offset = 0;
        int best_priority = -1;

        for (uint16_t i = 0; i < num_subtables; ++i) {
            uint32_t st_off = 4 + i * 8;
            if (st_off + 8 > cmap_len) break;
            uint16_t platform = read_u16(cmap_data + st_off);
            uint16_t encoding = read_u16(cmap_data + st_off + 2);
            uint32_t offset   = read_u32(cmap_data + st_off + 4);

            // Prefer: (3,10) format 12 > (3,1) format 4 > (0,3) format 4
            int priority = 0;
            if (platform == 3 && encoding == 10) priority = 3;
            else if (platform == 3 && encoding == 1) priority = 2;
            else if (platform == 0 && encoding >= 3) priority = 1;

            if (priority > best_priority) {
                best_priority = priority;
                best_offset = offset;
            }
        }

        if (best_offset > 0 && best_offset < cmap_len) {
            uint16_t format = read_u16(cmap_data + best_offset);

            if (format == 4) {
                // Format 4: Segment mapping to delta values
                if (best_offset + 14 <= cmap_len) {
                    uint16_t seg_count_x2 = read_u16(cmap_data + best_offset + 6);
                    uint16_t seg_count = seg_count_x2 / 2;
                    uint32_t base = best_offset + 14;

                    for (uint16_t seg = 0; seg < seg_count; ++seg) {
                        uint32_t end_off   = base + seg * 2;
                        uint32_t start_off = base + (seg_count + 1) * 2 + seg * 2;
                        uint32_t delta_off = base + (seg_count + 1) * 2 + seg_count * 2 + seg * 2;
                        uint32_t range_off = base + (seg_count + 1) * 2 + seg_count * 4 + seg * 2;

                        if (range_off + 2 > cmap_len) break;

                        uint16_t end_code   = read_u16(cmap_data + end_off);
                        uint16_t start_code = read_u16(cmap_data + start_off);
                        int16_t  id_delta   = read_i16(cmap_data + delta_off);
                        uint16_t id_range   = read_u16(cmap_data + range_off);

                        if (start_code == 0xFFFF) break;

                        for (uint32_t c = start_code; c <= end_code; ++c) {
                            uint16_t glyph_index;
                            if (id_range == 0) {
                                glyph_index = static_cast<uint16_t>(
                                    (c + static_cast<uint32_t>(id_delta)) & 0xFFFF);
                            } else {
                                uint32_t glyph_offset = range_off +
                                    id_range + (c - start_code) * 2;
                                if (glyph_offset + 2 > cmap_len) continue;
                                glyph_index = read_u16(cmap_data + glyph_offset);
                                if (glyph_index != 0) {
                                    glyph_index = static_cast<uint16_t>(
                                        (glyph_index + static_cast<uint32_t>(id_delta)) & 0xFFFF);
                                }
                            }
                            if (glyph_index != 0) {
                                parsed.cmap[c] = glyph_index;
                            }
                        }
                    }
                }
            } else if (format == 12) {
                // Format 12: Segmented coverage (32-bit)
                if (best_offset + 16 <= cmap_len) {
                    uint32_t num_groups = read_u32(cmap_data + best_offset + 12);
                    for (uint32_t g = 0; g < num_groups; ++g) {
                        uint32_t grp_off = best_offset + 16 + g * 12;
                        if (grp_off + 12 > cmap_len) break;
                        uint32_t start_code  = read_u32(cmap_data + grp_off);
                        uint32_t end_code    = read_u32(cmap_data + grp_off + 4);
                        uint32_t start_glyph = read_u32(cmap_data + grp_off + 8);
                        for (uint32_t c = start_code; c <= end_code; ++c) {
                            uint16_t gi = static_cast<uint16_t>(start_glyph + (c - start_code));
                            if (gi != 0) parsed.cmap[c] = gi;
                        }
                    }
                }
            }
        }
    }

    parsed_.push_back(std::move(parsed));
    return true;
}

bool FontLoader::parse_cmap(const uint8_t* /*data*/, size_t /*size*/,
                             FontTableEntry& /*entry*/) {
    // Cmap parsing is done inline in parse_font_tables for simplicity
    return true;
}

bool FontLoader::parse_head(const uint8_t* /*data*/, size_t /*size*/,
                             FontTableEntry& /*entry*/) {
    // Head parsing is done inline in parse_font_tables
    return true;
}

bool FontLoader::parse_horizontal_metrics(const uint8_t* /*data*/, size_t /*size*/,
                                           FontTableEntry& /*entry*/) {
    // Horizontal metrics parsing is done inline in parse_font_tables
    return true;
}

} // namespace pictor
