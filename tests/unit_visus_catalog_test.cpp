/// VisusCatalog — name 索引 / ディレクトリ読込 / children 解決 / 循環・深さ検査。

#include "pictor/visus/visus_catalog.h"
#include "pictor/visus/visus_serializer.h"
#include "test_common.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace pictor;
using namespace pictor_test;

namespace fs = std::filesystem;

namespace {

VisusDesc make(const char* name, VisusKind kind, std::vector<std::string> children = {}) {
    VisusDesc d;
    d.name = name;
    d.kind = kind;
    for (std::string& c : children) {
        VisusChildRef r;
        r.visus = std::move(c);
        d.children.push_back(std::move(r));
    }
    return d;
}

bool any_contains(const std::vector<std::string>& v, const char* needle) {
    return std::any_of(v.begin(), v.end(), [&](const std::string& s) {
        return s.find(needle) != std::string::npos;
    });
}

void test_add_find_duplicate() {
    VisusCatalog cat;
    PT_ASSERT(cat.add(make("a", VisusKind::MODEL)), "add a");
    std::string err;
    PT_ASSERT(!cat.add(make("a", VisusKind::RIVE), {}, &err), "duplicate name rejected");
    PT_ASSERT(err.find("duplicate") != std::string::npos, "duplicate error message");
    PT_ASSERT(!cat.add(make("", VisusKind::RIVE), {}, &err), "empty name rejected");
    PT_ASSERT(cat.add(make("b", VisusKind::RIVE), "same/source.visus.json", &err),
              "first source path accepted");
    PT_ASSERT(!cat.add(make("c", VisusKind::RIVE), "same/source.visus.json", &err),
              "duplicate source path rejected");
    PT_ASSERT(cat.find("a") && cat.find("a")->kind == VisusKind::MODEL, "find keeps first");
    PT_ASSERT(cat.find("zzz") == nullptr, "missing");
    PT_ASSERT_OP(cat.size(), ==, size_t{2}, "size");
    PT_ASSERT(cat.remove("a") && cat.size() == 1, "remove");
}

void test_children_resolution_and_validate() {
    VisusCatalog cat;
    cat.add(make("kuzuha", VisusKind::MODEL, {"kuzuha_facial", "katana"}));
    cat.add(make("kuzuha_facial", VisusKind::RIVE));
    cat.add(make("katana", VisusKind::MODEL));
    PT_ASSERT(cat.validate().empty(), "healthy tree validates clean");

    std::string err;
    const VisusDesc* child = cat.resolve_child(*cat.find("kuzuha"), cat.find("kuzuha")->children[0], &err);
    PT_ASSERT(child && child->name == "kuzuha_facial", "name reference resolves");

    cat.add(make("broken", VisusKind::GROUP, {"missing_child"}));
    const auto issues = cat.validate();
    PT_ASSERT(any_contains(issues, "missing_child"), "unresolved child reported");
}

void test_cycle_detection() {
    VisusCatalog cat;
    cat.add(make("a", VisusKind::GROUP, {"b"}));
    cat.add(make("b", VisusKind::GROUP, {"c"}));
    cat.add(make("c", VisusKind::GROUP, {"a"}));
    const auto issues = cat.validate();
    PT_ASSERT(any_contains(issues, "visus cycle"), "cycle detected");
    PT_ASSERT(any_contains(issues, "a -> b -> c -> a"), "cycle path reported from a");

    VisusCatalog self;
    self.add(make("s", VisusKind::GROUP, {"s"}));
    PT_ASSERT(any_contains(self.validate(), "visus cycle: s -> s"), "self cycle detected");
}

void test_load_directory_reports_validation_errors() {
    std::error_code ec;
    const fs::path dir = fs::temp_directory_path(ec) / "pictor_unit_visus_catalog_cycle";
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);

    auto write = [&](const fs::path& p, const VisusDesc& d) {
        std::ofstream f(p, std::ios::binary);
        f << to_visus_json(d);
    };
    write(dir / "a.visus.json", make("a", VisusKind::GROUP, {"b"}));
    write(dir / "b.visus.json", make("b", VisusKind::GROUP, {"a"}));

    VisusCatalog cat;
    std::vector<std::string> errors;
    PT_ASSERT_OP(cat.load_directory(dir.generic_string(), &errors), ==, size_t{2},
                 "syntactically valid cycle files are read");
    PT_ASSERT(any_contains(errors, "visus cycle"), "load reports cycle validation failure");
    fs::remove_all(dir, ec);
}

void test_loaded_path_ref_does_not_fall_back_to_unrelated_name() {
    VisusCatalog cat;
    VisusDesc parent = make("parent", VisusKind::GROUP, {"children/child.visus.json"});
    PT_ASSERT(cat.add(std::move(parent), "catalog/parent.visus.json"), "parent added with source");
    PT_ASSERT(cat.add(make("child", VisusKind::MODEL), "elsewhere/child.visus.json"),
              "same-stem decoy added elsewhere");

    std::string err;
    const VisusDesc* resolved = cat.resolve_child(*cat.find("parent"), cat.find("parent")->children[0], &err);
    PT_ASSERT(resolved == nullptr, "loaded path ref requires exact source path");
    PT_ASSERT(err.find("not loaded") != std::string::npos, "missing exact path is reported");
}

void test_depth_limit() {
    VisusCatalog cat;
    // 深さ kMaxChildDepth + 2 の直鎖: n0 -> n1 -> ... -> n10
    const int n = VisusCatalog::kMaxChildDepth + 2;
    for (int i = 0; i < n; ++i) {
        std::vector<std::string> ch;
        if (i + 1 < n) ch.push_back("n" + std::to_string(i + 1));
        cat.add(make(("n" + std::to_string(i)).c_str(), VisusKind::GROUP, ch));
    }
    std::vector<std::string> out;
    cat.validate_tree("n0", out);
    PT_ASSERT(any_contains(out, "nesting too deep"), "depth over limit reported");

    // ちょうど上限 (root + 8 段) は OK
    std::vector<std::string> ok;
    cat.validate_tree("n1", ok);   // n1..n9 = root + 8 段
    PT_ASSERT(ok.empty(), "depth at limit is fine");
}

void test_shared_subtree_validation_is_bounded() {
    VisusCatalog cat;
    constexpr int kBranching = 12;

    // 12-way repeated references over eight levels are a tiny catalog but a
    // naive tree walk expands to 12^8 visits. Each referenced definition only
    // needs validation once at a given depth.
    for (int i = 0; i <= VisusCatalog::kMaxChildDepth; ++i) {
        std::vector<std::string> children;
        if (i < VisusCatalog::kMaxChildDepth) {
            children.assign(kBranching, "shared" + std::to_string(i + 1));
        }
        cat.add(make(("shared" + std::to_string(i)).c_str(),
                     VisusKind::GROUP, std::move(children)));
    }

    std::vector<std::string> issues;
    cat.validate_tree("shared0", issues);
    PT_ASSERT(issues.empty(), "repeated shared subtrees validate without exponential expansion");
}

void test_load_directory_and_path_refs() {
    std::error_code ec;
    const fs::path dir = fs::temp_directory_path(ec) / "pictor_unit_visus_catalog";
    fs::remove_all(dir, ec);
    fs::create_directories(dir / "weapons", ec);

    auto write = [&](const fs::path& p, const VisusDesc& d) {
        std::ofstream f(p, std::ios::binary);
        f << to_visus_json(d);
    };
    {
        VisusDesc k = make("kuzuha", VisusKind::MODEL, {"kuzuha_facial", "weapons/katana.visus.json"});
        k.asset = "../models/kuzuha.fbx";
        write(dir / "kuzuha.visus.json", k);
        write(dir / "kuzuha_facial.visus.json", make("kuzuha_facial", VisusKind::RIVE));
        write(dir / "weapons" / "katana.visus.json", make("katana", VisusKind::MODEL));
        // name がファイル名と違う → warning
        write(dir / "misnamed.visus.json", make("other_name", VisusKind::UI));
        // name 空 → ファイル名 stem が name
        write(dir / "anonymous.visus.json", make("", VisusKind::TEXT));
        // 壊れたファイル → errors
        std::ofstream bad(dir / "broken.visus.json", std::ios::binary);
        bad << "{ \"name\": ";
        // 拡張子違いは無視
        std::ofstream other(dir / "notes.json", std::ios::binary);
        other << "{}";
    }

    VisusCatalog cat;
    std::vector<std::string> errors, warnings;
    const size_t loaded = cat.load_directory(dir.generic_string(), &errors, &warnings);
    PT_ASSERT_OP(loaded, ==, size_t{4}, "4 valid files loaded (non-recursive, *.visus.json only)");
    PT_ASSERT(any_contains(errors, "broken.visus.json"), "broken file reported in errors");
    PT_ASSERT(any_contains(warnings, "differs from file stem"), "name/stem mismatch warned");
    PT_ASSERT(cat.contains("anonymous"), "empty name takes file stem");
    PT_ASSERT(cat.contains("other_name"), "mismatched name is kept as declared");
    PT_ASSERT(!cat.contains("katana"), "subdirectory not loaded by load_directory");

    // resolve_path は visus ファイル起点
    const std::string asset = cat.resolve_path(*cat.find("kuzuha"), cat.find("kuzuha")->asset);
    PT_ASSERT(asset.find("/models/kuzuha.fbx") != std::string::npos &&
              asset.find("pictor_unit_visus_catalog/models") == std::string::npos,
              "relative asset resolved against visus dir (../ applied)");

    // パス参照の子はまだ未読込 → validate が報告
    PT_ASSERT(any_contains(cat.validate(), "katana.visus.json"), "path child not loaded reported");

    // 子ファイルを読んだら解決できる
    std::string err;
    PT_ASSERT(cat.load_file((dir / "weapons" / "katana.visus.json").generic_string(), &err), "load child file");
    const VisusDesc* katana = cat.resolve_child(*cat.find("kuzuha"), cat.find("kuzuha")->children[1], &err);
    PT_ASSERT(katana && katana->name == "katana", "path reference resolves by source_path");
    PT_ASSERT(cat.validate().empty(), "all resolved after loading child");

    fs::remove_all(dir, ec);
}

} // namespace

int main() {
    test_add_find_duplicate();
    test_children_resolution_and_validate();
    test_cycle_detection();
    test_load_directory_reports_validation_errors();
    test_loaded_path_ref_does_not_fall_back_to_unrelated_name();
    test_depth_limit();
    test_shared_subtree_validation_is_bounded();
    test_load_directory_and_path_refs();
    return report("unit_visus_catalog_test");
}
