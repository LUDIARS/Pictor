// FBX Document -- Level 1 implementation
//
// Structure:
//   1. FBXProperty / FBXNode accessor implementations
//   2. Self-contained zlib inflate (RFC 1950 + RFC 1951 DEFLATE)
//   3. Binary FBX parser
//   4. ASCII FBX parser (tokenizer + recursive descent)
//   5. Public API (parse / parse_binary / parse_ascii / load_file)
//
// No third-party dependencies (std::* only).
// Failures set error message and return false; no exceptions thrown.

#include "pictor/animation/fbx_document.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace pictor {

namespace {

// ── 安全な little-endian 読み出し ─────────────────────────────

template <typename T>
inline T read_le(const uint8_t* p) noexcept {
    static_assert(std::is_trivially_copyable_v<T>);
    T v;
    std::memcpy(&v, p, sizeof(T));
    return v;
}

inline bool set_err(std::string* dst, std::string msg) {
    if (dst) *dst = std::move(msg);
    return false;
}

// ── zlib / DEFLATE 自前 inflate ──────────────────────────────────
//
// RFC 1950 (zlib): 2-byte header (CMF/FLG) + DEFLATE stream + 4-byte ADLER32
// RFC 1951 (DEFLATE): bitstream of blocks
//   Block header: BFINAL(1) + BTYPE(2)
//   BTYPE 00: stored (LEN/NLEN + raw bytes)
//   BTYPE 01: fixed Huffman
//   BTYPE 10: dynamic Huffman (literal/length tree + distance tree, encoded with code-length tree)
//
// 速度より正しさ優先。シンボルあたり 1 ビットずつ読みつつ canonical Huffman を辿る。

struct BitReader {
    const uint8_t* data;
    size_t         size;
    size_t         byte_pos = 0;
    int            bit_pos  = 0;     // 0..7, LSB-first within a byte
    bool           bad      = false;

    BitReader(const uint8_t* d, size_t s) : data(d), size(s) {}

    /// `n` ビットを LSB-first で読み出す (DEFLATE のビット順)。最大 16 ビット。
    uint32_t read_bits(int n) {
        uint32_t v = 0;
        for (int i = 0; i < n; ++i) {
            if (byte_pos >= size) { bad = true; return 0; }
            uint32_t bit = (data[byte_pos] >> bit_pos) & 1u;
            v |= (bit << i);
            if (++bit_pos == 8) { bit_pos = 0; ++byte_pos; }
        }
        return v;
    }

    void align_to_byte() {
        if (bit_pos != 0) { bit_pos = 0; ++byte_pos; }
    }

    bool read_bytes(uint8_t* dst, size_t n) {
        align_to_byte();
        if (byte_pos + n > size) { bad = true; return false; }
        std::memcpy(dst, data + byte_pos, n);
        byte_pos += n;
        return true;
    }
};

// canonical Huffman 表 (シンボル → コード長から構築)
struct HuffTable {
    int max_bits = 0;
    std::array<int, 16> bl_count{};                // 各長さのコード数
    std::array<int, 16> next_code{};               // 長さ毎の最初のコード値 (>>1 ベース)
    std::vector<int>    sorted_symbols;            // (len, value) でソートされたシンボル
    std::array<int, 16> first_symbol_index{};      // 各長さの開始インデックス
    std::array<int, 16> first_code{};              // 各長さの最初のコード値
    bool ok = false;

    bool build(const std::vector<int>& lengths) {
        bl_count.fill(0);
        next_code.fill(0);
        first_symbol_index.fill(0);
        first_code.fill(0);
        sorted_symbols.clear();

        int n = static_cast<int>(lengths.size());
        max_bits = 0;
        for (int len : lengths) {
            if (len < 0 || len > 15) return false;
            if (len > 0) {
                ++bl_count[len];
                if (len > max_bits) max_bits = len;
            }
        }
        if (max_bits == 0) {
            // 0 長 (= 表が空)。許容する (使われないことが前提)。
            ok = true;
            return true;
        }

        // canonical code 値 (RFC 1951 §3.2.2 アルゴリズム)
        int code = 0;
        bl_count[0] = 0;
        for (int bits = 1; bits <= max_bits; ++bits) {
            code = (code + bl_count[bits - 1]) << 1;
            next_code[bits] = code;
            first_code[bits] = code;
        }

        // ソート済みシンボル列を作る (長さ昇順、同じ長さの中ではシンボル昇順)
        sorted_symbols.resize(n);
        std::vector<int> bl_offset(16, 0);
        int acc = 0;
        for (int bits = 1; bits <= max_bits; ++bits) {
            first_symbol_index[bits] = acc;
            bl_offset[bits] = acc;
            acc += bl_count[bits];
        }
        for (int sym = 0; sym < n; ++sym) {
            int len = lengths[sym];
            if (len > 0) sorted_symbols[bl_offset[len]++] = sym;
        }
        sorted_symbols.resize(acc);

        ok = true;
        return true;
    }

    /// 1 シンボル分のビットを読んでデコード。失敗時 -1。
    int decode(BitReader& br) const {
        if (max_bits == 0) return -1;
        int code = 0;
        for (int bits = 1; bits <= max_bits; ++bits) {
            code = (code << 1) | static_cast<int>(br.read_bits(1));
            if (br.bad) return -1;
            int count = bl_count[bits];
            if (count > 0) {
                int delta = code - first_code[bits];
                if (delta >= 0 && delta < count) {
                    return sorted_symbols[first_symbol_index[bits] + delta];
                }
            }
        }
        return -1;
    }
};

// length / distance テーブル (RFC 1951 §3.2.5)
static const int kLengthBase[29] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
    35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258
};
static const int kLengthExtra[29] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
    3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0
};
static const int kDistBase[30] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
    257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577
};
static const int kDistExtra[30] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
    7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13
};
// dynamic Huffman の code-length 順序
static const int kCodeLengthOrder[19] = {
    16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
};

static void build_fixed_tables(HuffTable& lit, HuffTable& dist) {
    // RFC 1951 §3.2.6 固定 Huffman のコード長
    std::vector<int> lit_lengths(288);
    for (int i = 0;   i <= 143; ++i) lit_lengths[i] = 8;
    for (int i = 144; i <= 255; ++i) lit_lengths[i] = 9;
    for (int i = 256; i <= 279; ++i) lit_lengths[i] = 7;
    for (int i = 280; i <= 287; ++i) lit_lengths[i] = 8;
    lit.build(lit_lengths);

    std::vector<int> dist_lengths(30, 5);
    dist.build(dist_lengths);
}

static bool inflate_block_data(BitReader& br, const HuffTable& lit, const HuffTable& dist,
                               std::vector<uint8_t>& out, std::string* error) {
    while (true) {
        int sym = lit.decode(br);
        if (sym < 0) return set_err(error,"deflate: bad literal/length symbol");
        if (sym < 256) {
            out.push_back(static_cast<uint8_t>(sym));
        } else if (sym == 256) {
            return true; // end of block
        } else {
            int li = sym - 257;
            if (li < 0 || li > 28) return set_err(error,"deflate: bad length code");
            int length = kLengthBase[li] + static_cast<int>(br.read_bits(kLengthExtra[li]));
            int dsym = dist.decode(br);
            if (dsym < 0 || dsym > 29) return set_err(error,"deflate: bad distance symbol");
            int distance = kDistBase[dsym] + static_cast<int>(br.read_bits(kDistExtra[dsym]));
            if (br.bad) return set_err(error,"deflate: read past end");
            if (distance <= 0 || static_cast<size_t>(distance) > out.size()) {
                return set_err(error,"deflate: distance exceeds output buffer");
            }
            // back-reference (distance > length も許容、1 バイトずつコピー)
            size_t start = out.size() - static_cast<size_t>(distance);
            for (int i = 0; i < length; ++i) out.push_back(out[start + i]);
        }
    }
}

static bool decode_dynamic_tables(BitReader& br, HuffTable& lit, HuffTable& dist, std::string* error) {
    int hlit  = static_cast<int>(br.read_bits(5)) + 257;
    int hdist = static_cast<int>(br.read_bits(5)) + 1;
    int hclen = static_cast<int>(br.read_bits(4)) + 4;
    if (br.bad) return set_err(error,"deflate: header read fail");
    if (hlit  > 286) return set_err(error,"deflate: hlit too large");
    if (hdist >  30) return set_err(error,"deflate: hdist too large");

    std::vector<int> code_length_lengths(19, 0);
    for (int i = 0; i < hclen; ++i) {
        code_length_lengths[kCodeLengthOrder[i]] = static_cast<int>(br.read_bits(3));
    }
    HuffTable cl_table;
    if (!cl_table.build(code_length_lengths)) {
        return set_err(error,"deflate: bad code-length table");
    }

    int total = hlit + hdist;
    std::vector<int> lengths(total, 0);
    int idx = 0;
    while (idx < total) {
        int sym = cl_table.decode(br);
        if (sym < 0) return set_err(error,"deflate: bad code-length symbol");
        if (sym <= 15) {
            lengths[idx++] = sym;
        } else if (sym == 16) {
            int rep = 3 + static_cast<int>(br.read_bits(2));
            if (idx == 0) return set_err(error,"deflate: repeat with no previous");
            int prev = lengths[idx - 1];
            for (int i = 0; i < rep && idx < total; ++i) lengths[idx++] = prev;
        } else if (sym == 17) {
            int rep = 3 + static_cast<int>(br.read_bits(3));
            for (int i = 0; i < rep && idx < total; ++i) lengths[idx++] = 0;
        } else if (sym == 18) {
            int rep = 11 + static_cast<int>(br.read_bits(7));
            for (int i = 0; i < rep && idx < total; ++i) lengths[idx++] = 0;
        } else {
            return set_err(error,"deflate: bad symbol in code-length stream");
        }
    }

    std::vector<int> lit_lens(lengths.begin(), lengths.begin() + hlit);
    std::vector<int> dist_lens(lengths.begin() + hlit, lengths.end());
    if (!lit.build(lit_lens) || !dist.build(dist_lens)) {
        return set_err(error,"deflate: bad dynamic tree");
    }
    return true;
}

static bool inflate_stream(const uint8_t* in, size_t in_size,
                           std::vector<uint8_t>& out, size_t expected_out_size,
                           std::string* error) {
    BitReader br(in, in_size);
    if (expected_out_size > 0) out.reserve(expected_out_size);

    bool last = false;
    while (!last) {
        last = (br.read_bits(1) != 0);
        uint32_t btype = br.read_bits(2);
        if (br.bad) return set_err(error,"deflate: header EOF");

        if (btype == 0) {
            // stored
            br.align_to_byte();
            if (br.byte_pos + 4 > br.size) return set_err(error,"deflate: stored EOF");
            uint16_t len  = read_le<uint16_t>(in + br.byte_pos);
            uint16_t nlen = read_le<uint16_t>(in + br.byte_pos + 2);
            br.byte_pos += 4;
            if (static_cast<uint16_t>(~len) != nlen) {
                return set_err(error,"deflate: stored len/nlen mismatch");
            }
            if (br.byte_pos + len > br.size) return set_err(error,"deflate: stored body EOF");
            out.insert(out.end(), in + br.byte_pos, in + br.byte_pos + len);
            br.byte_pos += len;
        } else if (btype == 1) {
            HuffTable lit, dist;
            build_fixed_tables(lit, dist);
            if (!inflate_block_data(br, lit, dist, out, error)) return false;
        } else if (btype == 2) {
            HuffTable lit, dist;
            if (!decode_dynamic_tables(br, lit, dist, error)) return false;
            if (!inflate_block_data(br, lit, dist, out, error)) return false;
        } else {
            return set_err(error,"deflate: reserved BTYPE 11");
        }
    }
    return true;
}

} // namespace

// ── 公開 zlib API ──────────────────────────────────────────────

bool fbx_zlib_decompress(const uint8_t* in, size_t in_size,
                         std::vector<uint8_t>& out,
                         size_t expected_out_size,
                         std::string* error) {
    out.clear();
    if (in_size < 6) return set_err(error,"zlib: too small");
    uint8_t cmf = in[0];
    uint8_t flg = in[1];
    if ((cmf & 0x0F) != 8) return set_err(error,"zlib: not deflate");
    if (((static_cast<uint32_t>(cmf) << 8) | flg) % 31u != 0) {
        return set_err(error,"zlib: bad header checksum");
    }
    if (flg & 0x20) return set_err(error,"zlib: preset dictionary not supported");
    // payload は header の 2 バイト後から、末尾 4 バイトの ADLER32 まで
    // (ADLER32 検証は省略 — FBX では破損検知は file-level で行うため)
    return inflate_stream(in + 2, in_size - 2 - 4, out, expected_out_size, error);
}

// ── FBXProperty アクセサ ──────────────────────────────────────

bool FBXProperty::is_scalar() const noexcept {
    switch (type) {
        case FBXPropertyType::BOOL:
        case FBXPropertyType::INT16:
        case FBXPropertyType::INT32:
        case FBXPropertyType::INT64:
        case FBXPropertyType::FLOAT:
        case FBXPropertyType::DOUBLE:
            return true;
        default: return false;
    }
}

bool FBXProperty::is_array() const noexcept {
    switch (type) {
        case FBXPropertyType::ARR_BOOL:
        case FBXPropertyType::ARR_INT32:
        case FBXPropertyType::ARR_INT64:
        case FBXPropertyType::ARR_FLOAT:
        case FBXPropertyType::ARR_DOUBLE:
            return true;
        default: return false;
    }
}

bool FBXProperty::is_string() const noexcept {
    return type == FBXPropertyType::STRING || type == FBXPropertyType::RAW;
}

size_t FBXProperty::array_size() const noexcept {
    switch (type) {
        case FBXPropertyType::ARR_BOOL:   return arr_i32.size();  // bool は i32 領域に 0/1 で詰める
        case FBXPropertyType::ARR_INT32:  return arr_i32.size();
        case FBXPropertyType::ARR_INT64:  return arr_i64.size();
        case FBXPropertyType::ARR_FLOAT:  return arr_f32.size();
        case FBXPropertyType::ARR_DOUBLE: return arr_f64.size();
        default: return 0;
    }
}

bool FBXProperty::as_bool() const noexcept {
    if (is_scalar()) return i64 != 0 || f64 != 0.0;
    if (is_string()) return !str.empty();
    return false;
}

int64_t FBXProperty::as_int() const noexcept {
    switch (type) {
        case FBXPropertyType::BOOL:
        case FBXPropertyType::INT16:
        case FBXPropertyType::INT32:
        case FBXPropertyType::INT64: return i64;
        case FBXPropertyType::FLOAT:
        case FBXPropertyType::DOUBLE: return static_cast<int64_t>(f64);
        case FBXPropertyType::STRING:
        case FBXPropertyType::RAW: {
            try { return std::stoll(str); } catch (...) { return 0; }
        }
        default: return 0;
    }
}

double FBXProperty::as_double() const noexcept {
    switch (type) {
        case FBXPropertyType::BOOL:
        case FBXPropertyType::INT16:
        case FBXPropertyType::INT32:
        case FBXPropertyType::INT64: return static_cast<double>(i64);
        case FBXPropertyType::FLOAT:
        case FBXPropertyType::DOUBLE: return f64;
        case FBXPropertyType::STRING:
        case FBXPropertyType::RAW: {
            try { return std::stod(str); } catch (...) { return 0.0; }
        }
        default: return 0.0;
    }
}

std::string FBXProperty::as_string() const {
    if (is_string()) return str;
    if (type == FBXPropertyType::FLOAT || type == FBXPropertyType::DOUBLE) {
        std::ostringstream oss; oss << f64; return oss.str();
    }
    if (is_scalar()) return std::to_string(i64);
    return {};
}

std::vector<double> FBXProperty::as_double_array() const {
    std::vector<double> r;
    switch (type) {
        case FBXPropertyType::ARR_DOUBLE: return arr_f64;
        case FBXPropertyType::ARR_FLOAT:
            r.reserve(arr_f32.size());
            for (float v : arr_f32) r.push_back(static_cast<double>(v));
            return r;
        case FBXPropertyType::ARR_INT32:
            r.reserve(arr_i32.size());
            for (int32_t v : arr_i32) r.push_back(static_cast<double>(v));
            return r;
        case FBXPropertyType::ARR_INT64:
            r.reserve(arr_i64.size());
            for (int64_t v : arr_i64) r.push_back(static_cast<double>(v));
            return r;
        case FBXPropertyType::ARR_BOOL:
            r.reserve(arr_i32.size());
            for (int32_t v : arr_i32) r.push_back(v ? 1.0 : 0.0);
            return r;
        default: return r;
    }
}

std::vector<int64_t> FBXProperty::as_int_array() const {
    std::vector<int64_t> r;
    switch (type) {
        case FBXPropertyType::ARR_INT64: return arr_i64;
        case FBXPropertyType::ARR_INT32:
            r.reserve(arr_i32.size());
            for (int32_t v : arr_i32) r.push_back(static_cast<int64_t>(v));
            return r;
        case FBXPropertyType::ARR_BOOL:
            r.reserve(arr_i32.size());
            for (int32_t v : arr_i32) r.push_back(v ? 1 : 0);
            return r;
        case FBXPropertyType::ARR_FLOAT:
            r.reserve(arr_f32.size());
            for (float v : arr_f32) r.push_back(static_cast<int64_t>(v));
            return r;
        case FBXPropertyType::ARR_DOUBLE:
            r.reserve(arr_f64.size());
            for (double v : arr_f64) r.push_back(static_cast<int64_t>(v));
            return r;
        default: return r;
    }
}

// ── FBXNode ナビゲーション ────────────────────────────────────

const FBXNode* FBXNode::find_child(std::string_view n) const noexcept {
    for (const auto& c : children) if (c.name == n) return &c;
    return nullptr;
}

std::vector<const FBXNode*> FBXNode::find_children(std::string_view n) const {
    std::vector<const FBXNode*> r;
    for (const auto& c : children) if (c.name == n) r.push_back(&c);
    return r;
}

const FBXNode* FBXNode::find_descendant(std::string_view path) const noexcept {
    while (!path.empty() && path.front() == '/') path.remove_prefix(1);
    if (path.empty()) return this;
    auto slash = path.find('/');
    std::string_view head = (slash == std::string_view::npos) ? path : path.substr(0, slash);
    std::string_view tail = (slash == std::string_view::npos) ? std::string_view{} : path.substr(slash + 1);
    const FBXNode* c = find_child(head);
    if (!c) return nullptr;
    return c->find_descendant(tail);
}

const FBXProperty* FBXNode::child_property(std::string_view child_name, size_t index) const noexcept {
    const FBXNode* c = find_child(child_name);
    if (!c || index >= c->properties.size()) return nullptr;
    return &c->properties[index];
}

const FBXProperty* FBXNode::property(size_t index) const noexcept {
    if (index >= properties.size()) return nullptr;
    return &properties[index];
}

// ── バイナリ FBX パーサー ──────────────────────────────────────
//
// ヘッダ:
//   "Kaydara FBX Binary  \x00\x1A\x00" (21 + "  " + 3 = 23 ... 実測 27 バイト)
//   uint32 version (LE)
// ノードヘッダ:
//   version >= 7500: 64bit (end_offset, num_props, prop_list_len) + uint8 name_len + name
//   version <  7500: 32bit (end_offset, num_props, prop_list_len) + uint8 name_len + name
// プロパティ: 1 byte type + data
// 子ノードリスト終端: 全ゼロのノードヘッダ (sentinel)

namespace {

struct BinReader {
    const uint8_t* data;
    size_t         size;
    size_t         pos = 0;
    bool           ok  = true;

    BinReader(const uint8_t* d, size_t s) : data(d), size(s) {}

    bool read_bytes(void* dst, size_t n) {
        if (pos + n > size) { ok = false; return false; }
        std::memcpy(dst, data + pos, n);
        pos += n;
        return true;
    }
    template <typename T> bool read_le(T& v) { return read_bytes(&v, sizeof(T)); }
    bool skip(size_t n) {
        if (pos + n > size) { ok = false; return false; }
        pos += n; return true;
    }
};

bool read_property(BinReader& br, FBXProperty& p, std::string* error) {
    uint8_t code = 0;
    if (!br.read_le(code)) return set_err(error,"binary: prop type EOF");
    p.type = static_cast<FBXPropertyType>(code);
    switch (p.type) {
        case FBXPropertyType::BOOL: {
            uint8_t v; if (!br.read_le(v)) return set_err(error,"binary: BOOL EOF");
            p.i64 = (v != 0) ? 1 : 0;
            return true;
        }
        case FBXPropertyType::INT16: {
            int16_t v; if (!br.read_le(v)) return set_err(error,"binary: INT16 EOF");
            p.i64 = v; return true;
        }
        case FBXPropertyType::INT32: {
            int32_t v; if (!br.read_le(v)) return set_err(error,"binary: INT32 EOF");
            p.i64 = v; return true;
        }
        case FBXPropertyType::INT64: {
            int64_t v; if (!br.read_le(v)) return set_err(error,"binary: INT64 EOF");
            p.i64 = v; return true;
        }
        case FBXPropertyType::FLOAT: {
            float v; if (!br.read_le(v)) return set_err(error,"binary: FLOAT EOF");
            p.f64 = static_cast<double>(v); return true;
        }
        case FBXPropertyType::DOUBLE: {
            double v; if (!br.read_le(v)) return set_err(error,"binary: DOUBLE EOF");
            p.f64 = v; return true;
        }
        case FBXPropertyType::STRING:
        case FBXPropertyType::RAW: {
            uint32_t len; if (!br.read_le(len)) return set_err(error,"binary: STR len EOF");
            if (br.pos + len > br.size) return set_err(error,"binary: STR body EOF");
            p.str.assign(reinterpret_cast<const char*>(br.data + br.pos), len);
            br.pos += len;
            return true;
        }
        case FBXPropertyType::ARR_BOOL:
        case FBXPropertyType::ARR_INT32:
        case FBXPropertyType::ARR_INT64:
        case FBXPropertyType::ARR_FLOAT:
        case FBXPropertyType::ARR_DOUBLE: {
            uint32_t length, encoding, comp_len;
            if (!br.read_le(length))   return set_err(error,"binary: ARR length EOF");
            if (!br.read_le(encoding)) return set_err(error,"binary: ARR encoding EOF");
            if (!br.read_le(comp_len)) return set_err(error,"binary: ARR complen EOF");

            size_t elem_size = 0;
            switch (p.type) {
                case FBXPropertyType::ARR_BOOL:   elem_size = 1; break;
                case FBXPropertyType::ARR_INT32:  elem_size = 4; break;
                case FBXPropertyType::ARR_INT64:  elem_size = 8; break;
                case FBXPropertyType::ARR_FLOAT:  elem_size = 4; break;
                case FBXPropertyType::ARR_DOUBLE: elem_size = 8; break;
                default: break;
            }
            size_t expected = static_cast<size_t>(length) * elem_size;

            std::vector<uint8_t> raw_storage;
            const uint8_t* raw = nullptr;
            size_t raw_n = 0;
            if (encoding == 0) {
                if (br.pos + comp_len > br.size) return set_err(error,"binary: ARR raw EOF");
                if (comp_len != expected)        return set_err(error,"binary: ARR raw size mismatch");
                raw = br.data + br.pos;
                raw_n = comp_len;
                br.pos += comp_len;
            } else if (encoding == 1) {
                if (br.pos + comp_len > br.size) return set_err(error,"binary: ARR zlib EOF");
                if (!fbx_zlib_decompress(br.data + br.pos, comp_len, raw_storage, expected, error)) {
                    return false;
                }
                if (raw_storage.size() != expected) return set_err(error,"binary: ARR inflate size mismatch");
                raw = raw_storage.data();
                raw_n = raw_storage.size();
                br.pos += comp_len;
            } else {
                return set_err(error,"binary: ARR unknown encoding");
            }
            (void)raw_n;

            switch (p.type) {
                case FBXPropertyType::ARR_BOOL:
                    p.arr_i32.resize(length);
                    for (uint32_t i = 0; i < length; ++i) p.arr_i32[i] = raw[i] ? 1 : 0;
                    break;
                case FBXPropertyType::ARR_INT32:
                    p.arr_i32.resize(length);
                    std::memcpy(p.arr_i32.data(), raw, length * 4);
                    break;
                case FBXPropertyType::ARR_INT64:
                    p.arr_i64.resize(length);
                    std::memcpy(p.arr_i64.data(), raw, length * 8);
                    break;
                case FBXPropertyType::ARR_FLOAT:
                    p.arr_f32.resize(length);
                    std::memcpy(p.arr_f32.data(), raw, length * 4);
                    break;
                case FBXPropertyType::ARR_DOUBLE:
                    p.arr_f64.resize(length);
                    std::memcpy(p.arr_f64.data(), raw, length * 8);
                    break;
                default: break;
            }
            return true;
        }
        default:
            return set_err(error,std::string("binary: unknown prop type 0x") +
                            std::to_string(static_cast<int>(code)));
    }
}

bool read_node(BinReader& br, FBXNode& out, uint32_t version, std::string* error) {
    bool is_v75 = version >= 7500;
    uint64_t end_offset = 0, num_props = 0, prop_list_len = 0;
    if (is_v75) {
        if (!br.read_le(end_offset))   return set_err(error,"binary: node header EOF");
        if (!br.read_le(num_props))    return set_err(error,"binary: node header EOF");
        if (!br.read_le(prop_list_len))return set_err(error,"binary: node header EOF");
    } else {
        uint32_t a = 0, b = 0, c = 0;
        if (!br.read_le(a) || !br.read_le(b) || !br.read_le(c))
            return set_err(error,"binary: node header EOF");
        end_offset = a; num_props = b; prop_list_len = c;
    }
    uint8_t name_len = 0;
    if (!br.read_le(name_len)) return set_err(error,"binary: name len EOF");

    // sentinel: all zeros
    if (end_offset == 0 && num_props == 0 && prop_list_len == 0 && name_len == 0) {
        out.name.clear();
        return true;
    }

    if (br.pos + name_len > br.size) return set_err(error,"binary: name EOF");
    out.name.assign(reinterpret_cast<const char*>(br.data + br.pos), name_len);
    br.pos += name_len;

    out.properties.resize(num_props);
    for (uint64_t i = 0; i < num_props; ++i) {
        if (!read_property(br, out.properties[i], error)) return false;
    }

    // 子ノードがある場合、ここまでの位置 != end_offset
    while (br.pos < end_offset) {
        FBXNode child;
        if (!read_node(br, child, version, error)) return false;
        if (child.empty()) break;       // sentinel に到達
        out.children.push_back(std::move(child));
    }
    if (br.pos != end_offset) {
        // パディングや読み残しがあれば end_offset に強制移動 (許容する)
        if (end_offset > br.size) return set_err(error,"binary: end_offset overruns");
        br.pos = static_cast<size_t>(end_offset);
    }
    return true;
}

} // namespace

bool FBXDocument::looks_binary(const uint8_t* data, size_t size) noexcept {
    static const char kMagic[] = "Kaydara FBX Binary  ";
    constexpr size_t kMagicLen = sizeof(kMagic) - 1; // 20
    if (size < kMagicLen + 7) return false;
    return std::memcmp(data, kMagic, kMagicLen) == 0;
}

bool FBXDocument::parse_binary(const uint8_t* data, size_t size, std::string* error) {
    is_ascii = false;
    root = FBXNode{};
    if (!looks_binary(data, size)) return set_err(error,"binary: bad magic");

    // ヘッダ: 20-byte magic + 0x1A 0x00 + uint32 version  (= 27 bytes)
    constexpr size_t kHeaderSize = 27;
    if (size < kHeaderSize) return set_err(error,"binary: header too short");
    version = read_le<uint32_t>(data + 23);

    BinReader br(data, size);
    br.pos = kHeaderSize;

    while (br.pos < size) {
        FBXNode child;
        if (!read_node(br, child, version, error)) return false;
        if (child.empty()) break;
        root.children.push_back(std::move(child));
    }
    return true;
}

// ── ASCII FBX パーサー ────────────────────────────────────────
//
// FBX ASCII の文法 (簡略):
//   Document := Node*
//   Node     := Identifier ":" PropList? Block?
//   PropList := Property ("," Property)*
//   Property := Number | String | "*" Integer Block? | Identifier
//   Block    := "{" Node* "}"
//   String   := '"' chars '"'
//   コメント : ";" 〜 行末

namespace {

struct AsciiReader {
    const uint8_t* data;
    size_t         size;
    size_t         pos = 0;
    int            line = 1;

    AsciiReader(const uint8_t* d, size_t s) : data(d), size(s) {}

    bool eof() const { return pos >= size; }
    char peek() const { return pos < size ? static_cast<char>(data[pos]) : '\0'; }
    char get()  { char c = peek(); if (!eof()) { ++pos; if (c == '\n') ++line; } return c; }

    void skip_ws() {
        while (!eof()) {
            char c = peek();
            if (c == ';') {                        // line comment
                while (!eof() && peek() != '\n') ++pos;
            } else if (c == ' ' || c == '\t' || c == '\r') {
                ++pos;
            } else if (c == '\n') {
                ++pos; ++line;
            } else {
                break;
            }
        }
    }

    bool match(char c) { skip_ws(); if (peek() == c) { get(); return true; } return false; }

    // 識別子: [A-Za-z_][A-Za-z0-9_\-|:.]*
    bool read_identifier(std::string& out) {
        skip_ws();
        size_t start = pos;
        if (eof()) return false;
        char c = peek();
        if (!(std::isalpha(static_cast<unsigned char>(c)) || c == '_')) return false;
        while (!eof()) {
            char ch = peek();
            if (std::isalnum(static_cast<unsigned char>(ch)) ||
                ch == '_' || ch == '-' || ch == '|' || ch == ':' || ch == '.') {
                ++pos;
            } else break;
        }
        out.assign(reinterpret_cast<const char*>(data + start), pos - start);
        return !out.empty();
    }

    bool read_string(std::string& out) {
        skip_ws();
        if (peek() != '"') return false;
        get(); // '"'
        std::string s;
        while (!eof()) {
            char c = get();
            if (c == '"') { out = std::move(s); return true; }
            if (c == '\\' && !eof()) { s.push_back(get()); continue; }
            s.push_back(c);
        }
        return false;
    }

    // 数値 (整数 or 浮動)。e/E/+/-/. に対応。
    bool read_number(std::string& out) {
        skip_ws();
        size_t start = pos;
        if (peek() == '+' || peek() == '-') ++pos;
        bool has_digit = false, has_dot = false, has_exp = false;
        while (!eof()) {
            char c = peek();
            if (std::isdigit(static_cast<unsigned char>(c))) { has_digit = true; ++pos; }
            else if (c == '.' && !has_dot && !has_exp) { has_dot = true; ++pos; }
            else if ((c == 'e' || c == 'E') && has_digit && !has_exp) {
                has_exp = true; ++pos;
                if (peek() == '+' || peek() == '-') ++pos;
            } else break;
        }
        if (!has_digit) { pos = start; return false; }
        out.assign(reinterpret_cast<const char*>(data + start), pos - start);
        return true;
    }
};

bool parse_node_ascii(AsciiReader& r, FBXNode& out, std::string* error);

bool parse_property_value(AsciiReader& r, FBXProperty& p, std::string* error) {
    r.skip_ws();
    char c = r.peek();
    if (c == '"') {
        std::string s;
        if (!r.read_string(s)) return set_err(error,"ascii: bad string");
        p.type = FBXPropertyType::STRING;
        p.str  = std::move(s);
        return true;
    }
    if (c == 'T' || c == 'Y' || c == 'W' || c == 'F' || c == 'N') {
        // bool 風 (T/Y=true, F/N=false, W=何か拡張)。真面目に handle するのは T/F のみ。
        std::string id;
        if (r.read_identifier(id)) {
            if (id == "T" || id == "Y") { p.type = FBXPropertyType::BOOL; p.i64 = 1; return true; }
            if (id == "F" || id == "N") { p.type = FBXPropertyType::BOOL; p.i64 = 0; return true; }
            // 独立 identifier として扱う (列挙値等)
            p.type = FBXPropertyType::STRING;
            p.str  = std::move(id);
            return true;
        }
    }
    if (c == '*') {
        // 配列: *N { a: v1,v2,... }  または *N {}
        r.get(); // '*'
        std::string n;
        if (!r.read_number(n)) return set_err(error,"ascii: bad array length");
        long long expected_count = 0;
        try { expected_count = std::stoll(n); } catch (...) { expected_count = 0; }
        p.type = FBXPropertyType::ARR_DOUBLE;
        if (r.match('{')) {
            // "a:" の後に値リスト
            r.skip_ws();
            std::string a_id;
            if (r.read_identifier(a_id)) {
                if (!r.match(':')) return set_err(error,"ascii: expected ':' after array key");
            }
            // カンマ区切りの数値列 (改行も区切り扱い)
            while (true) {
                r.skip_ws();
                if (r.peek() == '}') { r.get(); break; }
                if (r.peek() == ',') { r.get(); continue; }
                std::string nv;
                if (!r.read_number(nv)) return set_err(error,"ascii: expected number in array");
                try {
                    p.arr_f64.push_back(std::stod(nv));
                } catch (...) { p.arr_f64.push_back(0.0); }
            }
        }
        // expected_count が無視される場合もある (FBX SDK の出力で稀)
        (void)expected_count;
        return true;
    }
    if (c == '+' || c == '-' || c == '.' || std::isdigit(static_cast<unsigned char>(c))) {
        std::string n;
        if (!r.read_number(n)) return set_err(error,"ascii: bad number");
        if (n.find('.') != std::string::npos || n.find('e') != std::string::npos || n.find('E') != std::string::npos) {
            p.type = FBXPropertyType::DOUBLE;
            try { p.f64 = std::stod(n); } catch (...) { p.f64 = 0.0; }
        } else {
            p.type = FBXPropertyType::INT64;
            try { p.i64 = std::stoll(n); } catch (...) { p.i64 = 0; }
        }
        return true;
    }
    // identifier (列挙値等)
    std::string id;
    if (r.read_identifier(id)) {
        p.type = FBXPropertyType::STRING;
        p.str  = std::move(id);
        return true;
    }
    return set_err(error,std::string("ascii: unexpected char '") + c + "' at line " + std::to_string(r.line));
}

bool parse_node_ascii(AsciiReader& r, FBXNode& out, std::string* error) {
    r.skip_ws();
    if (r.eof()) { out = FBXNode{}; return true; }
    if (!r.read_identifier(out.name)) {
        return set_err(error,std::string("ascii: expected identifier at line ") + std::to_string(r.line));
    }
    r.skip_ws();
    if (r.peek() != ':') {
        return set_err(error,std::string("ascii: expected ':' after '") + out.name + "' at line " + std::to_string(r.line));
    }
    r.get(); // ':'

    // プロパティリスト (オプション)
    r.skip_ws();
    if (r.peek() != '{' && !r.eof()) {
        // プロパティが無い行 (Properties70: { ... } 等) は ":" の直後がそのまま "{"
        // それ以外はカンマ区切りで読む
        while (true) {
            r.skip_ws();
            if (r.peek() == '{' || r.eof()) break;
            FBXProperty p;
            if (!parse_property_value(r, p, error)) return false;
            out.properties.push_back(std::move(p));
            r.skip_ws();
            if (r.peek() == ',') { r.get(); continue; }
            break;
        }
    }

    // 子ブロック (オプション)
    r.skip_ws();
    if (r.peek() == '{') {
        r.get(); // '{'
        while (true) {
            r.skip_ws();
            if (r.peek() == '}') { r.get(); break; }
            if (r.eof()) return set_err(error,"ascii: unterminated block");
            FBXNode child;
            if (!parse_node_ascii(r, child, error)) return false;
            if (child.name.empty() && child.properties.empty() && child.children.empty()) continue;
            out.children.push_back(std::move(child));
        }
    }
    return true;
}

} // namespace

bool FBXDocument::parse_ascii(const uint8_t* data, size_t size, std::string* error) {
    is_ascii = true;
    version  = 0;
    root     = FBXNode{};

    AsciiReader r(data, size);
    while (true) {
        r.skip_ws();
        if (r.eof()) break;
        FBXNode node;
        if (!parse_node_ascii(r, node, error)) return false;
        if (node.name == "FBXVersion" && !node.properties.empty()) {
            version = static_cast<uint32_t>(node.properties[0].as_int());
        }
        root.children.push_back(std::move(node));
    }
    return true;
}

bool FBXDocument::parse(const uint8_t* data, size_t size, std::string* error) {
    if (!data || size == 0) return set_err(error,"fbx: empty buffer");
    if (looks_binary(data, size)) return parse_binary(data, size, error);
    return parse_ascii(data, size, error);
}

bool FBXDocument::load_file(const std::string& path, std::string* error) {
    std::ifstream f(path, std::ios::binary);
    if (!f.good()) return set_err(error,"fbx: cannot open file: " + path);
    f.seekg(0, std::ios::end);
    auto sz = f.tellg();
    if (sz <= 0) return set_err(error,"fbx: empty file");
    std::vector<uint8_t> buf(static_cast<size_t>(sz));
    f.seekg(0);
    f.read(reinterpret_cast<char*>(buf.data()), sz);
    if (!f) return set_err(error,"fbx: read failure");
    return parse(buf.data(), buf.size(), error);
}

} // namespace pictor
