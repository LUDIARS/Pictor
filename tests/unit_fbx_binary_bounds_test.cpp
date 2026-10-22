// FBX node end offsets must never rewind the binary reader.
#include "pictor/animation/fbx_document.h"
#include "test_common.h"

#include <cstdint>
#include <limits>
#include <string>
#include <vector>

using namespace pictor;
using namespace pictor_test;

namespace {

struct BinaryFixture {
    std::vector<uint8_t> bytes;
    size_t width;

    explicit BinaryFixture(uint32_t version) : width(version >= 7500 ? 8 : 4) {
        const char magic[] = "Kaydara FBX Binary  \0\x1a\0";
        bytes.assign(magic, magic + 23);
        append(version, 4);
    }

    void append(uint64_t value, size_t count) {
        for (size_t i = 0; i < count; ++i) {
            bytes.push_back(static_cast<uint8_t>(value >> (8 * i)));
        }
    }

    void end_at(size_t node, uint64_t end) {
        for (size_t i = 0; i < width; ++i) {
            bytes[node + i] = static_cast<uint8_t>(end >> (8 * i));
        }
    }

    size_t node(const std::string& name, bool property = false) {
        const size_t start = bytes.size();
        append(0, width);
        append(property ? 1 : 0, width);
        append(property ? 5 : 0, width);
        bytes.push_back(static_cast<uint8_t>(name.size()));
        bytes.insert(bytes.end(), name.begin(), name.end());
        if (property) {
            bytes.push_back('I');
            append(42, 4);
        }
        return start;
    }

    void sentinel() { bytes.resize(bytes.size() + 3 * width + 1, 0); }

    bool parse(FBXDocument& doc, std::string& error) const {
        return doc.parse(bytes.data(), bytes.size(), &error);
    }
};

void expect_rejected(const BinaryFixture& fixture) {
    FBXDocument doc;
    std::string error;
    PT_ASSERT(!fixture.parse(doc, error), "invalid node end must fail");
    PT_ASSERT(error.find("end_offset") != std::string::npos,
              "failure identifies the invalid node end");
}

void test_invalid_end_offsets(uint32_t version) {
    BinaryFixture base(version);
    const size_t node = base.node("Node");
    const size_t consumed = base.bytes.size();
    base.sentinel();
    // Includes zero with a non-empty name (not a sentinel), the exact loop
    // reproducer, and offsets inside the consumed header/name.
    for (uint64_t end : {uint64_t{0}, uint64_t{node}, uint64_t{node + 1},
                         uint64_t{consumed - 1}, uint64_t{base.bytes.size() + 1}}) {
        auto fixture = base;
        fixture.end_at(node, end);
        expect_rejected(fixture);
    }
    if (version >= 7500) {
        auto fixture = base;
        fixture.end_at(node, std::numeric_limits<uint64_t>::max());
        expect_rejected(fixture);
    }

    BinaryFixture property(version);
    const size_t property_node = property.node("Property", true);
    property.end_at(property_node, property.bytes.size() - 1);
    expect_rejected(property);

    BinaryFixture child(version);
    const size_t parent = child.node("Parent");
    const size_t child_node = child.node("Child");
    child.end_at(child_node, child.bytes.size());
    child.end_at(parent, child.bytes.size() - 1);
    expect_rejected(child);

    BinaryFixture sentinel(version);
    const size_t sentinel_parent = sentinel.node("Parent");
    const size_t sentinel_start = sentinel.bytes.size();
    sentinel.sentinel();
    sentinel.end_at(sentinel_parent, sentinel_start + 1);
    expect_rejected(sentinel);

    BinaryFixture nested(version);
    const size_t nested_parent = nested.node("Parent");
    const size_t bad_child = nested.node("Child");
    nested.end_at(bad_child, bad_child);
    nested.end_at(nested_parent, nested.bytes.size());
    expect_rejected(nested);
}

void test_valid_nodes(uint32_t version) {
    BinaryFixture fixture(version);
    const size_t leaf = fixture.node("Leaf", true);
    fixture.end_at(leaf, fixture.bytes.size());
    const size_t parent = fixture.node("Parent");
    const size_t child = fixture.node("Child");
    fixture.end_at(child, fixture.bytes.size());
    fixture.sentinel();
    // Forward padding is permitted after the child-list sentinel.
    fixture.bytes.insert(fixture.bytes.end(), {0xab, 0xcd, 0xef});
    fixture.end_at(parent, fixture.bytes.size());
    const size_t sibling = fixture.node("Sibling");
    fixture.sentinel();
    fixture.end_at(sibling, fixture.bytes.size());
    fixture.sentinel();

    FBXDocument doc;
    std::string error;
    PT_ASSERT(fixture.parse(doc, error), "valid nodes and padding must parse");
    PT_ASSERT_OP(doc.root.children.size(), ==, size_t{3}, "all siblings retained");
    const auto* leaf_node = doc.root.find_child("Leaf");
    PT_ASSERT(leaf_node != nullptr, "leaf retained");
    if (leaf_node && leaf_node->property(0)) {
        PT_ASSERT_OP(leaf_node->property(0)->as_int(), ==, int64_t{42}, "property retained");
    } else {
        PT_ASSERT(false, "leaf property retained");
    }
    const auto* parent_node = doc.root.find_child("Parent");
    PT_ASSERT(parent_node && parent_node->find_child("Child"), "nested child retained");

    BinaryFixture empty(version);
    empty.sentinel();
    FBXDocument empty_doc;
    PT_ASSERT(empty.parse(empty_doc, error), "top-level zero sentinel must parse");
    PT_ASSERT(empty_doc.root.children.empty(), "sentinel is not a node");
}

} // namespace

int main() {
    for (uint32_t version : {7400u, 7500u}) {
        test_invalid_end_offsets(version);
        test_valid_nodes(version);
    }
    return report("unit_fbx_binary_bounds_test");
}
