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

// V-8: 手書き JSON パーサの skip_value 相互再帰。
// 未知キーの値として `[[[[...]]]]` を与えると skip_value 経路に入る。
void test_json_skip_value_deep_nesting() {
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
    test_json_skip_value_deep_nesting();
    test_json_shallow_ok();
    test_fbx_ascii_deep_nesting();
    return report("unit_parser_dos_test");
}
