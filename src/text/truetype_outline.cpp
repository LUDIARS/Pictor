// @spec SPEC-PC-VECTOR-TEXT (spec/feature/subsystem/text.md)
#include "truetype_outline.h"
#include <algorithm>
#include <cmath>
#include <span>
#include <stdexcept>

namespace pictor::detail {
namespace {
using Bytes = std::span<const uint8_t>;
Bytes slice(Bytes bytes, size_t offset, size_t length) {
    if (offset > bytes.size() || length > bytes.size() - offset)
        throw std::invalid_argument("Truncated TrueType outline");
    return bytes.subspan(offset, length);
}
struct Cursor {
    Bytes bytes;
    size_t position{};
    uint8_t byte() { auto b = slice(bytes, position, 1)[0]; ++position; return b; }
    uint16_t word() { auto hi = byte(); return static_cast<uint16_t>((hi << 8) | byte()); }
    int signed_word() { auto v = word(); return v < 32768 ? v : static_cast<int>(v) - 65536; }
    uint32_t dword() { auto hi = word(); return (static_cast<uint32_t>(hi) << 16) | word(); }
    void skip(size_t length) { slice(bytes, position, length); position += length; }
};
uint32_t tag(char a, char b, char c, char d) {
    return (uint32_t(a) << 24) | (uint32_t(b) << 16) | (uint32_t(c) << 8) | uint32_t(d);
}
Bytes table(Bytes file, uint32_t wanted) {
    Cursor header{file}; header.skip(4); auto count = header.word(); header.skip(6);
    for (unsigned i = 0; i < count; ++i) {
        auto name = header.dword(); header.skip(4);
        auto offset = header.dword(); auto size = header.dword();
        if (name == wanted) return slice(file, offset, size);
    }
    throw std::invalid_argument("Vector text requires TrueType glyf/loca outlines");
}
struct Point { float x{}, y{}; bool on{}; };
struct Contours { std::vector<Point> points; std::vector<size_t> ends; };
constexpr size_t point_limit = 262144;
constexpr unsigned component_limit = 1024;

class Decoder {
public:
    explicit Decoder(Bytes file)
        : glyf(table(file, tag('g','l','y','f'))), loca(table(file, tag('l','o','c','a'))) {
        Cursor head{table(file, tag('h','e','a','d'))}; head.skip(50); format = head.signed_word();
        Cursor maxp{table(file, tag('m','a','x','p'))}; maxp.skip(4); count = maxp.word();
        if (format != 0 && format != 1) throw std::invalid_argument("Invalid loca format");
    }
    Contours decode(uint16_t index, unsigned depth = 0) {
        if (index >= count || depth > 16 || ++visited > component_limit)
            throw std::invalid_argument("Invalid or excessive composite glyph graph");
        Cursor locations{loca, size_t(index) * (format ? 4 : 2)};
        auto start = format ? locations.dword() : uint32_t(locations.word()) * 2;
        auto end = format ? locations.dword() : uint32_t(locations.word()) * 2;
        if (end < start) throw std::invalid_argument("Descending glyph offsets");
        auto block = slice(glyf, start, end - start);
        if (block.empty()) return {};
        Cursor cursor{block}; int number = cursor.signed_word(); cursor.skip(8);
        if (number >= 0) return simple(cursor, static_cast<unsigned>(number));
        return composite(cursor, depth);
    }
private:
    Bytes glyf, loca;
    int format{};
    unsigned count{}, visited{};
    static Contours simple(Cursor& cursor, unsigned number) {
        Contours result;
        if (!number) return result;
        for (unsigned i = 0; i < number; ++i) {
            size_t end = cursor.word();
            if (i && end <= result.ends.back()) throw std::invalid_argument("Invalid contour endpoints");
            result.ends.push_back(end);
        }
        auto size = result.ends.back() + 1;
        auto instructions = cursor.word(); cursor.skip(instructions);
        std::vector<uint8_t> flags; flags.reserve(size);
        while (flags.size() < size) {
            auto flag = cursor.byte(); size_t repeat = (flag & 8) ? size_t(cursor.byte()) + 1 : 1;
            if (repeat > size - flags.size()) throw std::invalid_argument("Invalid glyph flag repeat");
            flags.insert(flags.end(), repeat, flag);
        }
        result.points.resize(size);
        // Accumulate in a wide signed type: malformed deltas must not wrap.
        for (unsigned axis = 0; axis < 2; ++axis) {
            int value = 0;
            unsigned short_mask = axis ? 4 : 2, same_mask = axis ? 32 : 16;
            for (size_t i = 0; i < size; ++i) {
                if (flags[i] & short_mask) {
                    int delta = cursor.byte(); value += (flags[i] & same_mask) ? delta : -delta;
                } else if (!(flags[i] & same_mask)) value += cursor.signed_word();
                if (value < -32768 || value > 32767) throw std::invalid_argument("Invalid glyph coordinate");
                if (axis) result.points[i].y = static_cast<float>(value);
                else result.points[i].x = static_cast<float>(value);
                result.points[i].on = (flags[i] & 1) != 0;
            }
        }
        return result;
    }
    Contours composite(Cursor& cursor, unsigned depth) {
        Contours result;
        uint16_t flags{};
        do {
            flags = cursor.word(); auto index = cursor.word();
            bool xy = (flags & 2) != 0;
            auto argument = [&]() -> int {
                if (flags & 1) return xy ? cursor.signed_word() : cursor.word();
                auto b = cursor.byte(); return xy && b >= 128 ? int(b) - 256 : b;
            };
            int arg1 = argument(), arg2 = argument();
            float a = 1, b = 0, c = 0, d = 1;
            auto fixed = [&]() { return cursor.signed_word() / 16384.f; };
            unsigned transforms = ((flags & 8) != 0) + ((flags & 64) != 0) + ((flags & 128) != 0);
            if (transforms > 1 || (flags & 0x1800) == 0x1800)
                throw std::invalid_argument("Conflicting composite transforms");
            if (flags & 8) a = d = fixed();
            else if (flags & 64) { a = fixed(); d = fixed(); }
            else if (flags & 128) { a = fixed(); b = fixed(); c = fixed(); d = fixed(); }
            auto child = decode(index, depth + 1);
            for (auto& p : child.points) { float x = p.x; p.x = a*x + c*p.y; p.y = b*x + d*p.y; }
            float dx{}, dy{};
            if (xy) {
                dx = static_cast<float>(arg1); dy = static_cast<float>(arg2);
                if (flags & 0x800) { float x = dx; dx = a*x + c*dy; dy = b*x + d*dy; }
                // Unhinted font-unit path: ROUND_XY_TO_GRID is a raster hint,
                // not permission to round the resolution-independent outline.
            } else {
                if (size_t(arg1) >= result.points.size() || size_t(arg2) >= child.points.size())
                    throw std::invalid_argument("Unsupported phantom or invalid component point");
                dx = result.points[arg1].x - child.points[arg2].x;
                dy = result.points[arg1].y - child.points[arg2].y;
            }
            auto base = result.points.size();
            if (child.points.size() > point_limit - base) throw std::invalid_argument("Composite glyph too large");
            for (auto p : child.points) { p.x += dx; p.y += dy; result.points.push_back(p); }
            for (auto end : child.ends) result.ends.push_back(base + end);
        } while (flags & 32);
        if (flags & 256) { auto size = cursor.word(); cursor.skip(size); }
        return result;
    }
};

void append_contour(GlyphOutline& outline, std::span<const Point> points) {
    Point start;
    size_t first{}, end = points.size();
    if (points.front().on) { start = points.front(); first = 1; }
    else if (points.back().on) { start = points.back(); --end; }
    else start = {(points.front().x + points.back().x)*.5f, (points.front().y + points.back().y)*.5f, true};
    outline.path.push_back({SvgPathCommand::MOVE_TO, start.x, start.y});
    for (size_t i = first; i < end;) {
        const auto p = points[i++];
        if (p.on) { outline.path.push_back({SvgPathCommand::LINE_TO, p.x, p.y}); continue; }
        const auto next = i < end ? points[i] : start;
        const auto target = next.on ? next : Point{(p.x + next.x)*.5f, (p.y + next.y)*.5f, true};
        outline.path.push_back({SvgPathCommand::QUAD_TO, target.x, target.y, p.x, p.y});
        if (next.on && i < end) ++i;
        // An off-curve next point is still a control point for the next segment.
    }
    outline.path.push_back({SvgPathCommand::CLOSE});
}
}
GlyphOutline decode_truetype_outline(const FontTableEntry& font, uint16_t glyph, uint32_t codepoint) {
    auto contours = Decoder(font.raw_data).decode(glyph);
    GlyphOutline outline; outline.codepoint = codepoint; outline.em_size = float(font.metrics.units_per_em);
    size_t start = 0;
    for (auto end : contours.ends) {
        append_contour(outline, std::span<const Point>(contours.points).subspan(start, end - start + 1));
        start = end + 1;
    }
    return outline;
}
}
