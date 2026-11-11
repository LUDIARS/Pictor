// A minimal TrueType font built in memory for parser tests and fuzz seeds:
// head / hhea / maxp / hmtx / cmap (format 4, 'A' -> glyph 1) / loca (short) / glyf
// (glyph 0 empty, glyph 1 a triangle). num_glyphs and num_h_metrics are written as
// given, so a test can declare more horizontal metrics than glyphs.
#pragma once

#include <cstdint>
#include <utility>
#include <vector>

namespace pictor_test {

struct SyntheticFontOptions {
    uint16_t num_glyphs    = 2;
    uint16_t num_h_metrics = 2;
};

inline std::vector<uint8_t> make_synthetic_font(SyntheticFontOptions options = {}) {
    using Bytes = std::vector<uint8_t>;
    auto u16 = [](Bytes& b, uint32_t v) { b.push_back(uint8_t(v >> 8)); b.push_back(uint8_t(v)); };
    auto u32 = [&](Bytes& b, uint32_t v) { u16(b, v >> 16); u16(b, v & 0xFFFF); };

    Bytes head(54, 0);
    head[18] = 0x03; head[19] = 0xE8;                  // unitsPerEm 1000
    Bytes hhea(36, 0);
    hhea[4] = 0x03; hhea[5] = 0x20;                    // ascender 800
    hhea[6] = 0xFF; hhea[7] = 0x38;                    // descender -200
    hhea[34] = uint8_t(options.num_h_metrics >> 8); hhea[35] = uint8_t(options.num_h_metrics);
    Bytes maxp(6, 0);
    maxp[0] = 0x00; maxp[1] = 0x00; maxp[2] = 0x50; maxp[3] = 0x00;  // version 0.5
    maxp[4] = uint8_t(options.num_glyphs >> 8); maxp[5] = uint8_t(options.num_glyphs);
    // hmtx sized for the declared numbers, as a well-formed font would be.
    Bytes hmtx;
    for (uint32_t i = 0; i < options.num_h_metrics; ++i) { u16(hmtx, 500 + i); u16(hmtx, 10); }
    for (uint32_t i = options.num_h_metrics; i < options.num_glyphs; ++i) u16(hmtx, 20);

    Bytes cmap;
    u16(cmap, 0); u16(cmap, 1);                        // version, numTables
    u16(cmap, 3); u16(cmap, 1); u32(cmap, 12);         // (3,1) at offset 12
    u16(cmap, 4); u16(cmap, 32); u16(cmap, 0);         // format 4, length, language
    u16(cmap, 4); u16(cmap, 4); u16(cmap, 1); u16(cmap, 0);  // segCountX2=4, search fields
    u16(cmap, 0x41); u16(cmap, 0xFFFF);                // endCode
    u16(cmap, 0);                                      // reservedPad
    u16(cmap, 0x41); u16(cmap, 0xFFFF);                // startCode
    u16(cmap, uint16_t(1 - 0x41)); u16(cmap, 1);       // idDelta ('A' -> 1)
    u16(cmap, 0); u16(cmap, 0);                        // idRangeOffset

    Bytes glyf;
    u16(glyf, 1);                                      // numberOfContours
    for (int i = 0; i < 4; ++i) u16(glyf, 0);          // bbox
    u16(glyf, 2);                                      // endPtsOfContours[0]
    u16(glyf, 0);                                      // instructionLength
    glyf.insert(glyf.end(), {0x01, 0x01, 0x01});       // on-curve, word deltas
    u16(glyf, 0); u16(glyf, 500); u16(glyf, uint16_t(-250));  // x deltas
    u16(glyf, 0); u16(glyf, 0); u16(glyf, 700);               // y deltas
    if (glyf.size() % 2) glyf.push_back(0);
    Bytes loca;
    u16(loca, 0); u16(loca, 0);                        // glyph 0 empty
    for (uint32_t i = 1; i < options.num_glyphs; ++i) u16(loca, uint32_t(glyf.size() / 2));

    const std::pair<const char*, const Bytes*> tables[] = {
        {"cmap", &cmap}, {"glyf", &glyf}, {"head", &head}, {"hhea", &hhea},
        {"hmtx", &hmtx}, {"loca", &loca}, {"maxp", &maxp}};
    const uint16_t count = sizeof(tables) / sizeof(tables[0]);
    Bytes font;
    u32(font, 0x00010000); u16(font, count); u16(font, 0); u16(font, 0); u16(font, 0);
    uint32_t offset = 12 + 16u * count;
    for (const auto& [tag, data] : tables) {
        font.insert(font.end(), tag, tag + 4);
        u32(font, 0); u32(font, offset); u32(font, uint32_t(data->size()));
        offset += uint32_t((data->size() + 3) & ~size_t{3});
    }
    for (const auto& entry : tables) {
        const Bytes& data = *entry.second;
        font.insert(font.end(), data.begin(), data.end());
        font.resize((font.size() + 3) & ~size_t{3}, 0);
    }
    return font;
}

} // namespace pictor_test
