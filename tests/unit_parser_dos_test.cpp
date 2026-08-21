/// パーサ DoS ハードニング回帰テスト (review/2026-06-11 V-5 / V-8)。
///
/// 深くネストした入力を与えても「スタックオーバーフローでクラッシュせず、
/// false を返して安全に失敗する」ことを確認する。 上限は
/// pictor::parse_limits::kMaxNestingDepth。

#include "pictor/core/parse_limits.h"
#include "pictor/visus/visus_serializer.h"
#include "pictor/animation/fbx_document.h"
#include "test_common.h"

#include <string>

using namespace pictor;
using namespace pictor_test;

namespace {

// 深度上限を十分に超える nest を作る。
constexpr int kDeep = parse_limits::kMaxNestingDepth * 8;

// V-8: 手書き JSON パーサの相互再帰。 v2 パーサは未知キーの値も実体化する
// ので、 `[[[[...]]]]` は parse_value ↔ parse_array の再帰に入る。
void test_json_unknown_value_deep_nesting() {
    std::string json = "{\"_dos\":";
    json.append(kDeep, '[');
    json.append(kDeep, ']');
    json += "}";

    VisusDesc out;
    std::string err;
    // クラッシュしないこと自体が検証対象。 深すぎる入力は失敗扱い。
    bool ok = from_visus_json(json, out, &err);
    PT_ASSERT(!ok, "deeply-nested unknown value must fail, not crash");
}

// 正常系: 浅いネストはこれまで通り成功する (上限導入で回帰させない)。
void test_json_shallow_ok() {
    VisusDesc out;
    std::string err;
    bool ok = from_visus_json("{\"name\":\"x\",\"_extra\":[[1,2],[3,4]]}", out, &err);
    PT_ASSERT(ok, "shallow nesting must still parse");
}

// Visus v2: `metadata` は任意の JSON 値を保持して round-trip するため、
// skip ではなく実体化する経路にも深さ上限が効くことを確認する。
void test_json_metadata_deep_nesting() {
    std::string json = "{\"version\":2,\"name\":\"x\",\"metadata\":{\"deep\":";
    json.append(kDeep, '[');
    json.append(kDeep, ']');
    json += "}}";

    VisusDesc out;
    std::string err;
    bool ok = from_visus_json(json, out, &err);
    PT_ASSERT(!ok, "deeply-nested metadata value must fail, not crash");
    PT_ASSERT(err.find("nesting") != std::string::npos, "error names nesting depth");
}

// 正常系: 上限未満の深さの metadata は保持される。
void test_json_metadata_moderate_ok() {
    constexpr int kModerate = parse_limits::kMaxNestingDepth / 2;
    std::string json = "{\"version\":2,\"name\":\"x\",\"metadata\":{\"deep\":";
    json.append(kModerate, '[');
    json.append(kModerate, ']');
    json += "}}";

    VisusDesc out;
    std::string err;
    bool ok = from_visus_json(json, out, &err);
    PT_ASSERT(ok, "moderate nesting must parse");
    PT_ASSERT(out.metadata.has("deep"), "moderate nested metadata kept");
}

// Flat arrays avoid the recursion limit but can still amplify each tiny token
// into a heap-allocated VisusValue. The parser must cap materialized values.
void test_json_metadata_value_count_limit() {
    constexpr int kTooManyValues = 100'001;
    std::string json = "{\"version\":2,\"name\":\"x\",\"metadata\":{\"many\":[";
    for (int i = 0; i < kTooManyValues; ++i) {
        if (i != 0) json.push_back(',');
        json.push_back('0');
    }
    json += "]}}";

    VisusDesc out;
    std::string err;
    PT_ASSERT(!from_visus_json(json, out, &err), "excessive metadata values must fail");
    PT_ASSERT(err.find("too many") != std::string::npos, "error names value-count limit");
}

// Flat objects also escape the recursion limit. VisusMetadata keeps insertion
// order in a vector and `set` scans it, so an object with N distinct keys is
// O(N^2) to build — the parser must cap members per object, not just the total
// value count.
void test_json_metadata_object_member_limit() {
    constexpr int kMaxObjectMembers = 1024;   // visus_json.cpp の上限
    auto flat_object = [](int keys) {
        std::string json = "{\"version\":2,\"name\":\"x\",\"metadata\":{";
        for (int i = 0; i < keys; ++i) {
            if (i != 0) json.push_back(',');
            json += "\"k" + std::to_string(i) + "\":0";
        }
        json += "}}";
        return json;
    };

    VisusDesc out;
    std::string err;
    PT_ASSERT(!from_visus_json(flat_object(kMaxObjectMembers + 1), out, &err),
              "excessive object members must fail");
    PT_ASSERT(err.find("too many object members") != std::string::npos,
              "error names the member-count limit");

    // 上限ちょうどは通る (規約 key はせいぜい 20 個なので余裕は十分)。
    PT_ASSERT(from_visus_json(flat_object(kMaxObjectMembers), out, &err),
              "object at the member limit still parses");
    PT_ASSERT_OP(out.metadata.size(), ==, size_t{kMaxObjectMembers}, "all members kept");
}

// V-5: FBX ASCII パーサ parse_node_ascii の無制限再帰。
void test_fbx_ascii_deep_nesting() {
    // `A: { A: { ... } }` を深くネスト。
    std::string fbx;
    for (int i = 0; i < kDeep; ++i) fbx += "A: {\n";
    for (int i = 0; i < kDeep; ++i) fbx += "}\n";

    FBXDocument doc;
    std::string err;
    bool ok = doc.parse_ascii(reinterpret_cast<const uint8_t*>(fbx.data()),
                              fbx.size(), &err);
    PT_ASSERT(!ok, "deeply-nested FBX ASCII must fail, not crash");
}

} // namespace

int main() {
    test_json_unknown_value_deep_nesting();
    test_json_shallow_ok();
    test_json_metadata_deep_nesting();
    test_json_metadata_moderate_ok();
    test_json_metadata_value_count_limit();
    test_json_metadata_object_member_limit();
    test_fbx_ascii_deep_nesting();
    return report("unit_parser_dos_test");
}
