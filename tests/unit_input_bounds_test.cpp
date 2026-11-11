/// 入力経路の上限チェックの境界テスト (job cpp-lorica-20261111、spec/plan/2026-11-11-lorica-adoption.md §6)。
///
/// 各上限について「ちょうど」は通り「+1」は断られること、壊れた入力と巨大な入力
/// (宣言だけ巨大なもの) が確保の前に断られることを確かめる。 ファイルの上限は
/// 疎ファイル (resize_file) で「上限 +1」の大きさを作るので、 実際に巨大な確保はしない。

#include "pictor/animation/bvh_importer.h"
#include "pictor/animation/fbx_document.h"
#include "pictor/animation/lottie_animation.h"
#include "pictor/animation/rive_animation.h"
#include "pictor/animation/vector_animation.h"
#include "pictor/core/bounded_file.h"
#include "pictor/core/parse_limits.h"
#include "pictor/gi/gi_bake.h"
#include "pictor/gi/gi_lighting_system.h"
#include "pictor/gpu/gpu_buffer_manager.h"
#include "pictor/memory/memory_subsystem.h"
#include "pictor/pipeline/pipeline_profile_serializer.h"
#include "pictor/scene/scene_registry.h"
#include "pictor/text/font_loader.h"
#include "synthetic_font.h"
#include "test_common.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace pictor;
using namespace pictor_test;
namespace fs = std::filesystem;

namespace {

fs::path temp_path(const char* name) {
    return fs::temp_directory_path() / (std::string("pictor_input_bounds_") + name);
}

void write_file(const fs::path& path, const std::vector<uint8_t>& bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

// A sparse file of the given size: no disk blocks, no memory.
fs::path sparse_file(const char* name, std::uintmax_t size) {
    const fs::path path = temp_path(name);
    write_file(path, {});
    fs::resize_file(path, size);
    return path;
}

// ---- read_file_bounded ------------------------------------------------------

void test_read_file_bounded() {
    const fs::path path = temp_path("exact.bin");
    write_file(path, std::vector<uint8_t>(100, 0xAB));
    std::vector<uint8_t> out;
    PT_ASSERT(read_file_bounded(path.string(), 100, out) == FileReadStatus::OK, "size == limit is read");
    PT_ASSERT_OP(out.size(), ==, size_t{100}, "whole file read");
    PT_ASSERT(read_file_bounded(path.string(), 99, out) == FileReadStatus::TOO_LARGE, "limit + 1 is refused");
    PT_ASSERT(out.empty(), "refused read leaves nothing");
    std::string text;
    PT_ASSERT(read_text_file_bounded(path.string(), 100, text) == FileReadStatus::OK, "text: size == limit");
    PT_ASSERT(read_text_file_bounded(path.string(), 99, text) == FileReadStatus::TOO_LARGE, "text: limit + 1");
    PT_ASSERT(read_file_bounded((path.string() + ".missing"), 100, out) == FileReadStatus::OPEN_FAILED,
              "missing file");
    write_file(path, {});
    PT_ASSERT(read_file_bounded(path.string(), 0, out) == FileReadStatus::OK, "empty file within a zero limit");
    PT_ASSERT(out.empty(), "empty file gives no bytes");
    std::error_code ec;
    fs::remove(path, ec);
}

// ---- FBX: document budget ---------------------------------------------------

struct FbxBinary {
    std::vector<uint8_t> bytes;
    FbxBinary() {
        const char magic[] = "Kaydara FBX Binary  \0\x1a\0";
        bytes.assign(magic, magic + 23);
        le(7400, 4);
    }
    void le(uint64_t v, size_t n) {
        for (size_t i = 0; i < n; ++i) bytes.push_back(i < 8 ? static_cast<uint8_t>(v >> (8 * i)) : 0);
    }
    void patch(size_t at, uint64_t v) {
        for (size_t i = 0; i < 4; ++i) bytes[at + i] = static_cast<uint8_t>(v >> (8 * i));
    }
    // One top-level node "N" holding `props` INT32 properties, then the sentinel.
    static FbxBinary with_int_props(uint32_t props) {
        FbxBinary f;
        const size_t node = f.bytes.size();
        f.le(0, 4); f.le(props, 4); f.le(props * 5u, 4);
        f.bytes.push_back(1); f.bytes.push_back('N');
        for (uint32_t i = 0; i < props; ++i) { f.bytes.push_back('I'); f.le(i, 4); }
        f.patch(node, f.bytes.size());
        f.le(0, 13);
        return f;
    }
    // One node with a raw INT32 array of `count` elements.
    static FbxBinary with_int_array(uint32_t count) {
        FbxBinary f;
        const size_t node = f.bytes.size();
        f.le(0, 4); f.le(1, 4); f.le(13 + count * 4u, 4);
        f.bytes.push_back(1); f.bytes.push_back('A');
        f.bytes.push_back('i'); f.le(count, 4); f.le(0, 4); f.le(count * 4u, 4);
        for (uint32_t i = 0; i < count; ++i) f.le(i, 4);
        f.patch(node, f.bytes.size());
        f.le(0, 13);
        return f;
    }
};

bool parse_with(const std::vector<uint8_t>& bytes, const FBXParseLimits& limits, std::string& error) {
    FBXDocument doc;
    doc.limits = limits;
    return doc.parse(bytes.data(), bytes.size(), &error);
}

void test_fbx_element_budget() {
    // One node + 4 properties = 5 elements.
    const auto doc = FbxBinary::with_int_props(4);
    FBXParseLimits limits;
    limits.max_elements = 5;
    std::string error;
    PT_ASSERT(parse_with(doc.bytes, limits, error), "elements == limit parses");
    limits.max_elements = 4;
    PT_ASSERT(!parse_with(doc.bytes, limits, error), "elements == limit + 1 is refused");
    PT_ASSERT(error.find("budget") != std::string::npos, "error names the budget");
}

void test_fbx_byte_budget() {
    const auto doc = FbxBinary::with_int_array(1000);
    FBXDocument probe;
    std::string error;
    PT_ASSERT(probe.parse(doc.bytes.data(), doc.bytes.size(), &error), "default limits parse the array");
    // Find the charged total by bisection-free reasoning: the default parse succeeded,
    // so search the smallest byte limit that still parses, then check limit - 1 fails.
    size_t lo = 0, hi = 1u << 20;
    while (lo < hi) {
        const size_t mid = lo + (hi - lo) / 2;
        FBXParseLimits limits;
        limits.max_bytes = mid;
        if (parse_with(doc.bytes, limits, error)) hi = mid; else lo = mid + 1;
    }
    PT_ASSERT(lo >= 4000, "the array's 4000 decoded bytes are charged");
    FBXParseLimits exact;
    exact.max_bytes = lo;
    PT_ASSERT(parse_with(doc.bytes, exact, error), "bytes == limit parses");
    exact.max_bytes = lo - 1;
    PT_ASSERT(!parse_with(doc.bytes, exact, error), "bytes == limit + 1 is refused");
}

void test_fbx_huge_declared_props() {
    // num_props larger than the element budget, in an input long enough to pass the
    // "one byte per property" check: refused before any property is allocated.
    FbxBinary f;
    const uint32_t declared = 1u << 20;
    f.le(0, 4); f.le(declared, 4); f.le(0, 4);
    f.bytes.push_back(1); f.bytes.push_back('N');
    f.bytes.resize(f.bytes.size() + declared + 16, 0);
    FBXParseLimits limits;
    limits.max_elements = 1000;
    std::string error;
    PT_ASSERT(!parse_with(f.bytes, limits, error), "declared count over the budget is refused");
    PT_ASSERT(error.find("budget") != std::string::npos, "refused by the budget, not by a later read");
}

void test_fbx_ascii_amplification() {
    // Two input bytes per property ("1,") would otherwise become ~170 bytes each.
    std::string ascii = "N: ";
    for (int i = 0; i < 2000; ++i) ascii += "1,";
    ascii += "1\n";
    const std::vector<uint8_t> bytes(ascii.begin(), ascii.end());
    FBXParseLimits limits;
    std::string error;
    PT_ASSERT(parse_with(bytes, limits, error), "default limits accept 2001 properties");
    limits.max_elements = 2002;  // the node + 2001 properties
    PT_ASSERT(parse_with(bytes, limits, error), "ASCII elements == limit parses");
    limits.max_elements = 2001;
    PT_ASSERT(!parse_with(bytes, limits, error), "ASCII elements == limit + 1 is refused");

    std::string array = "N: *4000 {\n a: ";
    for (int i = 0; i < 4000; ++i) array += "0,";
    array += "0\n}\n";
    const std::vector<uint8_t> array_bytes(array.begin(), array.end());
    FBXParseLimits small;
    small.max_bytes = 4000 * sizeof(double);
    PT_ASSERT(!parse_with(array_bytes, small, error), "ASCII array over the byte budget is refused");
}

void test_fbx_broken_inputs() {
    std::string error;
    FBXDocument doc;
    auto doc_bytes = FbxBinary::with_int_props(2).bytes;
    // Every cut fails except the 27-byte header alone (an empty document) and the
    // cut that only drops the final top-level sentinel.
    for (size_t cut = 23; cut < doc_bytes.size(); ++cut) {
        FBXDocument truncated;
        const bool accepted_cut = cut == 27 || cut == doc_bytes.size() - 13;
        PT_ASSERT(truncated.parse(doc_bytes.data(), cut, &error) == accepted_cut,
                  "a truncated binary document is refused");
    }
    // An array whose declared length overflows 32-bit length * element size.
    FbxBinary f;
    const size_t node = f.bytes.size();
    f.le(0, 4); f.le(1, 4); f.le(13, 4);
    f.bytes.push_back(1); f.bytes.push_back('A');
    f.bytes.push_back('d'); f.le(0xFFFFFFFFu, 4); f.le(0, 4); f.le(0, 4);
    f.patch(node, f.bytes.size());
    f.le(0, 13);
    PT_ASSERT(!doc.parse(f.bytes.data(), f.bytes.size(), &error), "oversized array declaration is refused");
}

void test_fbx_file_limit() {
    const fs::path path = sparse_file("big.fbx", std::uintmax_t{parse_limits::kMaxModelFileBytes} + 1);
    FBXDocument doc;
    std::string error;
    PT_ASSERT(!doc.load_file(path.string(), &error), "model file over the limit is refused");
    PT_ASSERT(error.find("too_large") != std::string::npos, "error says the file is too large");
    std::error_code ec;
    fs::remove(path, ec);
}

// ---- fonts -------------------------------------------------------------------

void test_font_hmtx_beyond_glyph_count() {
    // numberOfHMetrics larger than numGlyphs used to index past advance_widths.
    const auto font = make_synthetic_font({2, 600});
    FontLoader loader;
    const FontHandle handle = loader.load_from_memory(font.data(), font.size(), "bad_hmtx");
    PT_ASSERT(handle != INVALID_FONT, "font with excess h-metrics still loads (clamped)");
    GlyphMetrics metrics;
    PT_ASSERT(loader.get_glyph_metrics(handle, 'A', 1000.0f, metrics), "glyph A resolves");
    PT_ASSERT(metrics.advance_x == 501.0f, "advance of glyph 1 read from hmtx");

    const auto good = make_synthetic_font();
    const FontHandle ok = loader.load_from_memory(good.data(), good.size(), "good");
    PT_ASSERT(ok != INVALID_FONT, "well-formed synthetic font loads");
    for (size_t cut = 0; cut < good.size(); cut += 7) {
        FontLoader l;
        (void)l.load_from_memory(good.data(), cut, "truncated");  // must not crash
    }
}

void test_font_file_limit() {
    const fs::path path = sparse_file("big.ttf", std::uintmax_t{parse_limits::kMaxFontFileBytes} + 1);
    FontLoader loader;
    PT_ASSERT(loader.load_from_file(path.string()) == INVALID_FONT, "font file over the limit is refused");
    std::error_code ec;
    fs::remove(path, ec);
}

// ---- animation / vector / profile files ----------------------------------------

void test_animation_file_limits() {
    std::error_code ec;
    {
        const fs::path path = sparse_file("big.riv", std::uintmax_t{parse_limits::kMaxVectorAnimationFileBytes} + 1);
        RiveAnimation rive;
        PT_ASSERT(!rive.load_file(path.string()), "Rive file over the limit is refused");
        PT_ASSERT(rive.error().find("too_large") != std::string::npos, "Rive error says too large");
        fs::remove(path, ec);
    }
    {
        RiveAnimation rive;
        PT_ASSERT(!rive.load_file(temp_path("missing.riv").string()), "missing Rive file is refused");
    }
    {
        const fs::path path = sparse_file("big.json", std::uintmax_t{parse_limits::kMaxVectorAnimationFileBytes} + 1);
        LottieAnimation lottie;
        PT_ASSERT(!lottie.load_file(path.string()), "Lottie file over the limit is refused");
        fs::remove(path, ec);
    }
    {
        const fs::path path = sparse_file("big.bvh", std::uintmax_t{parse_limits::kMaxModelFileBytes} + 1);
        BVHImporter bvh;
        const BVHImportResult result = bvh.import_file(path.string());
        PT_ASSERT(!result.success, "BVH file over the limit is refused");
        fs::remove(path, ec);
    }
    {
        const fs::path path = sparse_file("big.profile.json", std::uintmax_t{parse_limits::kMaxConfigFileBytes} + 1);
        PipelineProfileDef profile;
        std::string error;
        PT_ASSERT(!load_pipeline_profile_file(path.string(), profile, &error), "profile file over the limit is refused");
        fs::remove(path, ec);
    }
}

// ---- SVG colours (std::from_chars instead of sscanf) ------------------------------

void test_svg_colors() {
    VectorAnimationPlayer player;
    PT_ASSERT(player.load_svg(R"(<svg viewBox="0 0 10 10"><path d="M 0 0 L 10 0 L 10 10 Z" )"
                              R"(fill="#ff8000" stroke="#zz0000"/></svg>)"),
              "SVG loads");
    const VectorFrame frame = player.evaluate();
    PT_ASSERT(frame.paths.size() == 1, "one path");
    if (frame.paths.size() == 1) {
        const VectorPath& path = frame.paths[0];
        PT_ASSERT(path.fill_color[0] == 1.0f && path.fill_color[1] == 128.0f / 255.0f &&
                  path.fill_color[2] == 0.0f, "#ff8000 parsed");
        PT_ASSERT(path.stroke_color[0] == 0.0f && path.stroke_color[3] == 1.0f,
                  "malformed #zz0000 keeps the default stroke");
    }
}

// ---- GI bake file ------------------------------------------------------------

void write_bake(const fs::path& path, uint32_t count, uint32_t flags, size_t payload) {
    std::vector<uint8_t> bytes(24, 0);
    const uint32_t header[4] = {0x50494342u, 1u, count, flags};
    std::memcpy(bytes.data(), header, sizeof(header));
    bytes.resize(bytes.size() + payload, 0);
    write_file(path, bytes);
}

void test_gi_bake_declared_count() {
    MemoryConfig mem_cfg;
    MemorySubsystem memory(mem_cfg);
    SceneRegistry registry(memory);
    GpuMemoryAllocator gpu_alloc;
    GPUBufferManager buffers(gpu_alloc);
    GILightingSystem gi(buffers, registry);
    GIBakeSystem bake(buffers, registry, gi);
    const fs::path path = temp_path("bake.pgib");
    std::error_code ec;

    // Four billion declared objects in a file with none: refused before resize.
    write_bake(path, 0xFFFFFFFFu, 0xFFFFFFFFu, 0);
    PT_ASSERT(!bake.load(path.string()).valid, "declared count larger than the file is refused");

    // Exactly the bytes the declared object ids need, and one byte short of them.
    write_bake(path, 3, 0, 3 * sizeof(ObjectId));
    const GIBakeResult exact = bake.load(path.string());
    PT_ASSERT(exact.valid, "count matching the payload loads");
    PT_ASSERT_OP(exact.object_ids.size(), ==, size_t{3}, "three object ids");
    write_bake(path, 3, 0, 3 * sizeof(ObjectId) - 1);
    PT_ASSERT(!bake.load(path.string()).valid, "payload one byte short is refused");
    write_bake(path, 3, 0, 3 * sizeof(ObjectId) + 1);
    PT_ASSERT(!bake.load(path.string()).valid, "trailing bytes are refused");
    fs::remove(path, ec);
}

} // namespace

int main() {
    test_read_file_bounded();
    test_fbx_element_budget();
    test_fbx_byte_budget();
    test_fbx_huge_declared_props();
    test_fbx_ascii_amplification();
    test_fbx_broken_inputs();
    test_fbx_file_limit();
    test_font_hmtx_beyond_glyph_count();
    test_font_file_limit();
    test_animation_file_limits();
    test_svg_colors();
    test_gi_bake_declared_count();
    return report("unit_input_bounds_test");
}
