// Fuzz entry: FBXDocument::parse (binary node tree, properties, arrays, the
// built-in zlib inflate, and the ASCII parser).
#include "fuzz_entry.h"

#include "pictor/animation/fbx_document.h"

#include <string>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    pictor::FBXDocument doc;
    std::string error;
    (void)doc.parse(data, size, &error);
    return 0;
}

namespace {

void le(std::vector<uint8_t>& b, uint64_t v, size_t n) {
    for (size_t i = 0; i < n; ++i) b.push_back(i < 8 ? static_cast<uint8_t>(v >> (8 * i)) : 0);
}

// A 7400 binary document: one node with an int, a string, a raw int32 array, a
// zlib (stored block) float array and an empty double array, one child node, then
// the sentinels.
std::vector<uint8_t> binary_seed() {
    const char magic[] = "Kaydara FBX Binary  \0\x1a\0";
    std::vector<uint8_t> b(magic, magic + 23);
    le(b, 7400, 4);
    const size_t node = b.size();
    le(b, 0, 4); le(b, 5, 4); le(b, 0, 4);  // end, num_props, prop_list_len (patched)
    b.push_back(4); b.insert(b.end(), {'N', 'o', 'd', 'e'});
    const size_t props = b.size();
    b.push_back('I'); le(b, 42, 4);
    b.push_back('S'); le(b, 3, 4); b.insert(b.end(), {'a', 'b', 'c'});
    b.push_back('i'); le(b, 2, 4); le(b, 0, 4); le(b, 8, 4); le(b, 7, 4); le(b, 9, 4);
    // zlib stream: header 78 01, one final stored block of 4 bytes, adler32.
    b.push_back('f'); le(b, 1, 4); le(b, 1, 4); le(b, 2 + 5 + 4 + 4, 4);
    b.insert(b.end(), {0x78, 0x01, 0x01, 0x04, 0x00, 0xFB, 0xFF, 0x00, 0x00, 0x80, 0x3F});
    le(b, 0, 4);
    // An empty raw double array: zero length must not reach memcpy with a null
    // destination (UBSan finding of the libFuzzer run, 2026-11-11).
    b.push_back('d'); le(b, 0, 4); le(b, 0, 4); le(b, 0, 4);
    const size_t prop_len = b.size() - props;
    const size_t child = b.size();
    le(b, 0, 4); le(b, 0, 4); le(b, 0, 4);
    b.push_back(5); b.insert(b.end(), {'C', 'h', 'i', 'l', 'd'});
    le(b, 0, 13);                           // child list sentinel
    const size_t child_end = b.size();
    le(b, 0, 13);                           // Node's child list sentinel
    const size_t node_end = b.size();
    le(b, 0, 13);                           // top-level sentinel
    auto patch = [&](size_t at, uint64_t v) { for (size_t i = 0; i < 4; ++i) b[at + i] = uint8_t(v >> (8 * i)); };
    patch(node, node_end);
    patch(node + 8, prop_len);
    patch(child, child_end);
    return b;
}

} // namespace

std::vector<std::vector<uint8_t>> pictor_fuzz_seeds() {
    const std::string ascii =
        "; FBX 7.4.0 project file\n"
        "FBXHeaderExtension:  {\n FBXVersion: 7400\n}\n"
        "Objects:  {\n Geometry: 1, \"Geometry::m\", \"Mesh\" {\n"
        "  Vertices: *6 {\n   a: 0,0,0,1,0,0\n  }\n"
        "  PolygonVertexIndex: *3 {\n   a: 0,1,-3\n  }\n }\n}\n"
        "Connections:  {\n C: \"OO\",1,0\n}\n";
    return {binary_seed(), std::vector<uint8_t>(ascii.begin(), ascii.end())};
}
