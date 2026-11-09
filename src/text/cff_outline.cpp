// @spec SPEC-PC-VECTOR-TEXT (spec/feature/subsystem/text.md)
// CFF (Compact Font Format, Adobe TN #5176) and Type 2 charstrings (TN #5177).
#include "cff_outline.h"

#include <cmath>
#include <cstdlib>
#include <span>
#include <string>
#include <stdexcept>
#include <vector>

namespace pictor::detail {
namespace {

using Bytes = std::span<const uint8_t>;

[[noreturn]] void fail(const char* what) { throw std::invalid_argument(what); }

Bytes slice(Bytes bytes, size_t offset, size_t length) {
    if (offset > bytes.size() || length > bytes.size() - offset) fail("Truncated CFF data");
    return bytes.subspan(offset, length);
}

uint32_t read_be(Bytes bytes, size_t offset, size_t size) {
    auto b = slice(bytes, offset, size);
    uint32_t v = 0;
    for (auto x : b) v = (v << 8) | x;
    return v;
}

uint32_t tag(char a, char b, char c, char d) {
    return (uint32_t(a) << 24) | (uint32_t(b) << 16) | (uint32_t(c) << 8) | uint32_t(d);
}

/// The table's bytes, or an empty span when the font has none.
Bytes find_table(Bytes file, uint32_t wanted) {
    size_t directory = 0;
    if (read_be(file, 0, 4) == tag('t', 't', 'c', 'f')) directory = read_be(file, 12, 4);
    const uint32_t count = read_be(file, directory + 4, 2);
    for (uint32_t i = 0; i < count; ++i) {
        const size_t record = directory + 12 + size_t(i) * 16;
        if (read_be(file, record, 4) != wanted) continue;
        return slice(file, read_be(file, record + 8, 4), read_be(file, record + 12, 4));
    }
    return {};
}

/// CFF INDEX: count, offSize, offsets (1-based), data.
struct Index {
    Bytes data;
    std::vector<uint32_t> offsets;
    size_t end = 0;  // byte after the INDEX within the CFF table
    [[nodiscard]] size_t size() const { return offsets.empty() ? 0 : offsets.size() - 1; }
    [[nodiscard]] Bytes at(size_t i) const {
        if (i + 1 >= offsets.size() || offsets[i + 1] < offsets[i]) fail("CFF INDEX entry out of range");
        return slice(data, offsets[i] - 1, offsets[i + 1] - offsets[i]);
    }
};

Index read_index(Bytes cff, size_t offset) {
    Index index;
    const uint32_t count = read_be(cff, offset, 2);
    if (count == 0) {
        index.end = offset + 2;
        return index;
    }
    const uint32_t off_size = read_be(cff, offset + 2, 1);
    if (off_size < 1 || off_size > 4) fail("Invalid CFF offSize");
    index.offsets.resize(count + 1);
    for (uint32_t i = 0; i <= count; ++i) index.offsets[i] = read_be(cff, offset + 3 + size_t(i) * off_size, off_size);
    const size_t data_start = offset + 3 + size_t(count + 1) * off_size;
    index.data = slice(cff, data_start, index.offsets[count] - 1);
    index.end = data_start + index.offsets[count] - 1;
    return index;
}

/// A DICT's operands by operator (12 x is 1200 + x); only the first value list is kept.
struct Dict {
    std::vector<std::pair<int, std::vector<double>>> entries;
    [[nodiscard]] const std::vector<double>* get(int op) const {
        for (const auto& [k, v] : entries) {
            if (k == op) return &v;
        }
        return nullptr;
    }
};

Dict read_dict(Bytes bytes) {
    Dict dict;
    std::vector<double> operands;
    for (size_t i = 0; i < bytes.size();) {
        const int b0 = bytes[i];
        if (b0 <= 21) {
            int op = b0;
            ++i;
            if (b0 == 12) {
                if (i >= bytes.size()) fail("Truncated CFF DICT");
                op = 1200 + bytes[i++];
            }
            dict.entries.emplace_back(op, operands);
            operands.clear();
        } else if (b0 == 28) {
            operands.push_back(static_cast<int16_t>(read_be(bytes, i + 1, 2)));
            i += 3;
        } else if (b0 == 29) {
            operands.push_back(static_cast<int32_t>(read_be(bytes, i + 1, 4)));
            i += 5;
        } else if (b0 == 30) {  // real: nibbles until 0xf
            std::string text;
            ++i;
            bool done = false;
            while (!done && i < bytes.size()) {
                const int byte = bytes[i++];
                for (const int nibble : {byte >> 4, byte & 15}) {
                    if (nibble <= 9) text += char('0' + nibble);
                    else if (nibble == 0xa) text += '.';
                    else if (nibble == 0xb) text += 'E';
                    else if (nibble == 0xc) text += "E-";
                    else if (nibble == 0xe) text += '-';
                    else if (nibble == 0xf) { done = true; break; }
                }
            }
            operands.push_back(text.empty() ? 0.0 : std::strtod(text.c_str(), nullptr));
        } else if (b0 >= 32 && b0 <= 246) {
            operands.push_back(b0 - 139);
            ++i;
        } else if (b0 >= 247 && b0 <= 250) {
            operands.push_back((b0 - 247) * 256 + int(read_be(bytes, i + 1, 1)) + 108);
            i += 2;
        } else if (b0 >= 251 && b0 <= 254) {
            operands.push_back(-(b0 - 251) * 256 - int(read_be(bytes, i + 1, 1)) - 108);
            i += 2;
        } else {
            fail("Invalid CFF DICT byte");
        }
    }
    return dict;
}

int subr_bias(size_t count) { return count < 1240 ? 107 : count < 33900 ? 1131 : 32768; }

/// Local subrs of a Private DICT (offset and size from the Top / FD DICT).
Index private_subrs(Bytes cff, const Dict& top) {
    const auto* priv = top.get(18);
    if (priv == nullptr || priv->size() < 2) return {};
    const size_t size = static_cast<size_t>((*priv)[0]), offset = static_cast<size_t>((*priv)[1]);
    const Dict dict = read_dict(slice(cff, offset, size));
    const auto* subrs = dict.get(19);
    if (subrs == nullptr || subrs->empty()) return {};
    return read_index(cff, offset + static_cast<size_t>((*subrs)[0]));
}

/// Type 2 charstring interpreter writing the outline in font units.
class Charstring {
public:
    Charstring(const Index& global, const Index& local, GlyphOutline& out) : global_(global), local_(local), out_(out) {}

    void run(Bytes code, unsigned depth = 0) {
        if (depth > 10) fail("CFF subroutine nesting too deep");
        for (size_t i = 0; i < code.size() && !ended_;) {
            const int b0 = code[i];
            if (b0 >= 32 || b0 == 28) {
                i = operand(code, i);
                continue;
            }
            ++i;
            switch (b0) {
                case 1: case 3: case 18: case 23:  // hstem vstem hstemhm vstemhm
                    stems();
                    break;
                case 19: case 20:  // hintmask cntrmask
                    stems();
                    i += (hints_ + 7) / 8;
                    break;
                case 21:  // rmoveto
                    width(2);
                    move(at(0), at(1));
                    break;
                case 22:  // hmoveto
                    width(1);
                    move(at(0), 0);
                    break;
                case 4:  // vmoveto
                    width(1);
                    move(0, at(0));
                    break;
                case 5:  // rlineto
                    for (size_t k = 0; k + 1 < stack_.size(); k += 2) line(at(k), at(k + 1));
                    break;
                case 6: case 7: {  // hlineto vlineto: alternating
                    bool horizontal = b0 == 6;
                    for (size_t k = 0; k < stack_.size(); ++k, horizontal = !horizontal) {
                        horizontal ? line(at(k), 0) : line(0, at(k));
                    }
                    break;
                }
                case 8:  // rrcurveto
                    for (size_t k = 0; k + 5 < stack_.size(); k += 6) curve(at(k), at(k + 1), at(k + 2), at(k + 3), at(k + 4), at(k + 5));
                    break;
                case 24: {  // rcurveline
                    size_t k = 0;
                    for (; k + 7 < stack_.size(); k += 6) curve(at(k), at(k + 1), at(k + 2), at(k + 3), at(k + 4), at(k + 5));
                    if (k + 1 < stack_.size()) line(at(k), at(k + 1));
                    break;
                }
                case 25: {  // rlinecurve
                    size_t k = 0;
                    for (; k + 7 < stack_.size(); k += 2) line(at(k), at(k + 1));
                    if (k + 5 < stack_.size()) curve(at(k), at(k + 1), at(k + 2), at(k + 3), at(k + 4), at(k + 5));
                    break;
                }
                case 26: {  // vvcurveto
                    size_t k = 0;
                    float dx1 = 0;
                    if (stack_.size() % 4 == 1) dx1 = at(k++);
                    for (; k + 3 < stack_.size(); k += 4, dx1 = 0) curve(dx1, at(k), at(k + 1), at(k + 2), 0, at(k + 3));
                    break;
                }
                case 27: {  // hhcurveto
                    size_t k = 0;
                    float dy1 = 0;
                    if (stack_.size() % 4 == 1) dy1 = at(k++);
                    for (; k + 3 < stack_.size(); k += 4, dy1 = 0) curve(at(k), dy1, at(k + 1), at(k + 2), at(k + 3), 0);
                    break;
                }
                case 30: case 31: {  // vhcurveto hvcurveto: alternating, last may carry an extra delta
                    bool horizontal = b0 == 31;
                    const size_t n = stack_.size();
                    for (size_t k = 0; k + 3 < n; k += 4, horizontal = !horizontal) {
                        const float extra = (n - k == 5) ? at(k + 4) : 0.0f;
                        if (horizontal) curve(at(k), 0, at(k + 1), at(k + 2), extra, at(k + 3));
                        else curve(0, at(k), at(k + 1), at(k + 2), at(k + 3), extra);
                    }
                    break;
                }
                case 10: case 29: {  // callsubr callgsubr
                    if (stack_.empty()) fail("CFF subroutine call without index");
                    const Index& subrs = b0 == 10 ? local_ : global_;
                    const long index = static_cast<long>(stack_.back()) + subr_bias(subrs.size());
                    stack_.pop_back();
                    if (index < 0 || static_cast<size_t>(index) >= subrs.size()) fail("CFF subroutine out of range");
                    run(subrs.at(static_cast<size_t>(index)), depth + 1);
                    continue;  // the subroutine's operands stay on the stack
                }
                case 11:  // return
                    return;
                case 14:  // endchar
                    width(0);
                    close();
                    ended_ = true;
                    break;
                case 12: {  // escape: flex family; other ops ignored
                    if (i >= code.size()) fail("Truncated CFF escape");
                    flex(code[i++]);
                    break;
                }
                default:
                    break;  // reserved / unsupported: clear like an operator
            }
            stack_.clear();
        }
    }

    void finish() {
        if (!ended_) close();
    }

private:
    size_t operand(Bytes code, size_t i) {
        const int b0 = code[i];
        float v = 0;
        if (b0 == 28) {
            v = static_cast<int16_t>(read_be(code, i + 1, 2));
            i += 3;
        } else if (b0 <= 246) {
            v = static_cast<float>(b0 - 139);
            i += 1;
        } else if (b0 <= 250) {
            v = static_cast<float>((b0 - 247) * 256 + int(read_be(code, i + 1, 1)) + 108);
            i += 2;
        } else if (b0 <= 254) {
            v = static_cast<float>(-(b0 - 251) * 256 - int(read_be(code, i + 1, 1)) - 108);
            i += 2;
        } else {  // 16.16 fixed
            v = static_cast<float>(static_cast<int32_t>(read_be(code, i + 1, 4))) / 65536.0f;
            i += 5;
        }
        if (stack_.size() >= 513) fail("CFF stack overflow");
        stack_.push_back(v);
        return i;
    }

    float at(size_t k) const { return k < stack_.size() ? stack_[k] : 0.0f; }

    /// The optional width before the first stack-clearing operator: dropped.
    void width(size_t expected) {
        if (!width_seen_ && stack_.size() > expected) stack_.erase(stack_.begin());
        width_seen_ = true;
    }
    void stems() {
        if (!width_seen_ && stack_.size() % 2 == 1) stack_.erase(stack_.begin());
        width_seen_ = true;
        hints_ += static_cast<unsigned>(stack_.size() / 2);
    }

    void close() {
        if (open_) out_.path.push_back({SvgPathCommand::CLOSE});
        open_ = false;
    }
    void move(float dx, float dy) {
        close();
        x_ += dx;
        y_ += dy;
        out_.path.push_back({SvgPathCommand::MOVE_TO, x_, y_});
        open_ = true;
    }
    void line(float dx, float dy) {
        x_ += dx;
        y_ += dy;
        out_.path.push_back({SvgPathCommand::LINE_TO, x_, y_});
    }
    void curve(float dx1, float dy1, float dx2, float dy2, float dx3, float dy3) {
        const float c1x = x_ + dx1, c1y = y_ + dy1;
        const float c2x = c1x + dx2, c2y = c1y + dy2;
        x_ = c2x + dx3;
        y_ = c2y + dy3;
        SvgPathPoint p{SvgPathCommand::CUBIC_TO, x_, y_, c1x, c1y};
        p.cx2 = c2x;
        p.cy2 = c2y;
        out_.path.push_back(p);
    }
    void flex(int op) {
        const auto s = [&](size_t k) { return at(k); };
        switch (op) {
            case 35:  // flex: two curves
                curve(s(0), s(1), s(2), s(3), s(4), s(5));
                curve(s(6), s(7), s(8), s(9), s(10), s(11));
                break;
            case 34:  // hflex
                curve(s(0), 0, s(1), s(2), s(3), 0);
                curve(s(4), 0, s(5), -s(2), s(6), 0);
                break;
            case 36:  // hflex1
                curve(s(0), s(1), s(2), s(3), s(4), 0);
                curve(s(5), 0, s(6), s(7), s(8), -(s(1) + s(3) + s(7)));
                break;
            case 37: {  // flex1: the last delta goes along the larger axis
                const float dx = s(0) + s(2) + s(4) + s(6) + s(8), dy = s(1) + s(3) + s(5) + s(7) + s(9);
                curve(s(0), s(1), s(2), s(3), s(4), s(5));
                if (std::fabs(dx) > std::fabs(dy)) curve(s(6), s(7), s(8), s(9), s(10), -dy);
                else curve(s(6), s(7), s(8), s(9), -dx, s(10));
                break;
            }
            default:
                break;
        }
    }

    const Index& global_;
    const Index& local_;
    GlyphOutline& out_;
    std::vector<float> stack_;
    float x_ = 0, y_ = 0;
    unsigned hints_ = 0;
    bool width_seen_ = false, open_ = false, ended_ = false;
};

/// The FD a CID-keyed glyph uses (FDSelect format 0 or 3).
size_t fd_of(Bytes cff, size_t fdselect, uint16_t glyph, size_t glyphs) {
    const uint32_t format = read_be(cff, fdselect, 1);
    if (format == 0) return read_be(cff, fdselect + 1 + glyph, 1);
    if (format == 3) {
        const uint32_t ranges = read_be(cff, fdselect + 1, 2);
        for (uint32_t r = 0; r < ranges; ++r) {
            const size_t at = fdselect + 3 + size_t(r) * 3;
            const uint32_t first = read_be(cff, at, 2);
            const uint32_t next = r + 1 < ranges ? read_be(cff, at + 3, 2) : read_be(cff, fdselect + 3 + size_t(ranges) * 3, 2);
            if (glyph >= first && glyph < next) return read_be(cff, at + 2, 1);
        }
        return 0;
    }
    (void)glyphs;
    fail("Unsupported CFF FDSelect format");
}

}  // namespace

bool has_cff_outlines(const FontTableEntry& font) {
    return !find_table(font.raw_data, tag('C', 'F', 'F', ' ')).empty();
}

GlyphOutline decode_cff_outline(const FontTableEntry& font, uint16_t glyph, uint32_t codepoint) {
    const Bytes cff = find_table(font.raw_data, tag('C', 'F', 'F', ' '));
    if (cff.empty()) fail("Font has no CFF table");
    const size_t header_size = read_be(cff, 2, 1);
    const Index names = read_index(cff, header_size);
    const Index tops = read_index(cff, names.end);
    const Index strings = read_index(cff, tops.end);
    const Index global = read_index(cff, strings.end);
    if (tops.size() == 0) fail("CFF without a Top DICT");
    const Dict top = read_dict(tops.at(0));
    const auto* charstrings_offset = top.get(17);
    if (charstrings_offset == nullptr || charstrings_offset->empty()) fail("CFF without CharStrings");
    const Index charstrings = read_index(cff, static_cast<size_t>((*charstrings_offset)[0]));
    if (glyph >= charstrings.size()) fail("CFF glyph out of range");

    Index local;
    if (const auto* fdarray = top.get(1236); fdarray != nullptr && !fdarray->empty()) {  // CID-keyed
        const auto* fdselect = top.get(1237);
        if (fdselect == nullptr || fdselect->empty()) fail("CID CFF without FDSelect");
        const Index fds = read_index(cff, static_cast<size_t>((*fdarray)[0]));
        const size_t fd = fd_of(cff, static_cast<size_t>((*fdselect)[0]), glyph, charstrings.size());
        if (fd >= fds.size()) fail("CFF FD out of range");
        local = private_subrs(cff, read_dict(fds.at(fd)));
    } else {
        local = private_subrs(cff, top);
    }

    GlyphOutline outline;
    outline.codepoint = codepoint;
    outline.em_size = float(font.metrics.units_per_em);
    Charstring interpreter(global, local, outline);
    interpreter.run(charstrings.at(glyph));
    interpreter.finish();
    return outline;
}

}  // namespace pictor::detail
