/// VisusValue / VisusMetadata — 値セマンティクス・順序保持・アクセサ。

#include "pictor/visus/visus_metadata.h"
#include "pictor/visus/visus_serializer.h"
#include "test_common.h"

#include <string>
#include <utility>
#include <vector>

using namespace pictor;
using namespace pictor_test;

namespace {

void test_scalars() {
    VisusValue n;            PT_ASSERT(n.is_null(), "default is null");
    VisusValue b(true);      PT_ASSERT(b.is_bool() && b.as_bool(), "bool");
    VisusValue d(1.5);       PT_ASSERT(d.is_number() && d.as_number() == 1.5, "double");
    VisusValue i(7);         PT_ASSERT(i.is_number() && i.as_number() == 7.0, "int -> number, not bool");
    VisusValue s("abc");     PT_ASSERT(s.is_string() && s.as_string() == "abc", "const char* -> string, not bool");
    VisusValue sv(std::string_view("xy")); PT_ASSERT(sv.as_string() == "xy", "string_view");
    // 型違いアクセスは既定値
    PT_ASSERT(s.as_number() == 0.0 && !s.as_bool() && s.as_array().empty(), "mismatched access is default");
}

void test_ordered_metadata() {
    VisusMetadata m;
    m.set("zeta", 1);
    m.set("alpha", "a");
    m.set("mid", true);
    PT_ASSERT_OP(m.size(), ==, size_t{3}, "3 entries");
    std::vector<std::string> keys;
    for (const auto& [k, v] : m) keys.push_back(k);
    PT_ASSERT(keys[0] == "zeta" && keys[1] == "alpha" && keys[2] == "mid", "insertion order kept");

    m.set("alpha", "b");   // 置換は順序を保つ
    PT_ASSERT_OP(m.size(), ==, size_t{3}, "set on existing key replaces");
    PT_ASSERT(m.get_string("alpha").value_or("") == "b", "replaced value");
    PT_ASSERT(!m.get_string("zeta").has_value(), "get_string on number is nullopt");
    PT_ASSERT(m.get_number("zeta").value_or(0) == 1.0, "get_number");
    PT_ASSERT(m.get_bool("mid").value_or(false), "get_bool");
    PT_ASSERT(m.find("none") == nullptr, "missing key");
    PT_ASSERT(m.erase("zeta") && !m.has("zeta"), "erase first entry");
    PT_ASSERT(m.get_string("alpha").value_or("") == "b" &&
              m.get_bool("mid").value_or(false),
              "erase refreshes indexes for shifted entries");
    m.clear();
    PT_ASSERT(m.empty() && m.find("alpha") == nullptr, "clear resets entries and index");
}

void test_deep_copy() {
    VisusMetadata inner;
    inner.set("k", 1);
    VisusValue::Array arr;
    arr.emplace_back(1);
    arr.emplace_back(VisusValue(inner));
    VisusValue a(arr);
    VisusValue b = a;                       // copy
    b.mutable_array()[0] = VisusValue(2);   // b だけ変える
    PT_ASSERT(a.as_array()[0].as_number() == 1.0, "copy is deep (array)");
    PT_ASSERT(a.as_array()[1].as_object().get_number("k").value_or(0) == 1.0, "nested object copied");
    PT_ASSERT(a != b, "inequality after mutation");
    VisusValue c = a;
    PT_ASSERT(a == c, "equality of deep copies");
}

// move 後の source は「中身の無い array/object」ではなく null になる。
void test_move_leaves_null_source() {
    VisusValue::Array arr;
    arr.emplace_back(1);
    VisusValue src(std::move(arr));

    VisusValue moved(std::move(src));
    PT_ASSERT(moved.is_array() && moved.as_array().size() == 1, "move transfers payload");
    PT_ASSERT(src.is_null(), "moved-from value is null, not a hollow array");   // NOLINT

    VisusValue target(true);
    target = std::move(moved);
    PT_ASSERT(target.is_array() && target.as_array().size() == 1, "move assign transfers payload");
    PT_ASSERT(moved.is_null(), "move assignment nulls the source too");         // NOLINT
}

// パーサが埋めた metadata も lookup index を持つこと。 上の test_ordered_metadata
// は直接 `set()` を呼ぶだけなので、 JSON 経由で entries_ / index_ が揃うことは
// 別に押さえる。 index_ を更新し忘れると find() が常に nullptr を返し、 JSON 由来の
// VisusDesc が (name / kind / asset / parts / children ごと) 全部空になる。
void test_parsed_metadata_is_indexed() {
    const char* json = R"({
      "version": 2, "name": "x", "kind": "model", "asset": "a.fbx",
      "metadata": { "render.flags": 2, "animation.default": "Idle" }
    })";
    VisusDesc d;
    PT_ASSERT(from_visus_json(json, d), "parse succeeds");
    PT_ASSERT(d.name == "x" && d.kind == VisusKind::MODEL && d.asset == "a.fbx",
              "typed fields readable after parse (root lookup uses the index)");
    PT_ASSERT(d.metadata.get_number("render.flags").value_or(-1) == 2.0,
              "parsed metadata key is findable via index");
    PT_ASSERT(d.metadata.get_string("animation.default").value_or("") == "Idle",
              "second parsed key is findable");
    PT_ASSERT(d.metadata.erase("render.flags") && !d.metadata.has("render.flags"),
              "erase keeps parser-populated entries_ and index_ consistent");
    PT_ASSERT(d.metadata.get_string("animation.default").value_or("") == "Idle",
              "erase refreshes shifted parser-populated indexes");
}

void test_ordered_merge() {
    VisusMetadata base;
    base.set("first", 1);
    base.set("shared", "old");
    VisusMetadata over;
    over.set("shared", "new");
    over.set("last", 3);

    base.merge_from(over);
    std::vector<std::string> keys;
    for (const auto& [key, _] : base) keys.push_back(key);
    PT_ASSERT(keys == std::vector<std::string>({"first", "shared", "last"}),
              "merge preserves existing order and appends new keys");
    PT_ASSERT(base.get_string("shared").value_or("") == "new",
              "merge replaces existing values");
    // 追加された key も索引に載っていないと、 以降の find / get_* / set /
    // erase が全て miss する (読み経路は index_ しか見ない)。
    PT_ASSERT(base.get_number("last").value_or(-1) == 3.0,
              "merge indexes newly appended keys");
    base.set("last", 4);
    PT_ASSERT_OP(base.size(), ==, size_t{3}, "set on a merged key replaces, not duplicates");
    PT_ASSERT(base.erase("last") && !base.has("last"), "merged key is erasable");
}

} // namespace

int main() {
    test_scalars();
    test_ordered_metadata();
    test_deep_copy();
    test_move_leaves_null_source();
    test_parsed_metadata_is_indexed();
    test_ordered_merge();
    return report("unit_visus_metadata_test");
}
