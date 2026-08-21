/// VisusPackageCatalog — *.shaderpkg.json のディレクトリ読込 / name 索引 /
/// パッケージファイル起点のパス解決。

#include "pictor/visus/visus_package_catalog.h"
#include "pictor/visus/visus_package_serializer.h"
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

bool any_contains(const std::vector<std::string>& v, const char* needle) {
    return std::any_of(v.begin(), v.end(), [&](const std::string& s) {
        return s.find(needle) != std::string::npos;
    });
}

VisusShaderPackage make(const char* name, VisusShaderRef shader) {
    VisusShaderPackage p;
    p.name   = name;
    p.shader = std::move(shader);
    return p;
}

void write_file(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out << text;
}

fs::path temp_dir(const char* leaf) {
    std::error_code ec;
    const fs::path dir = fs::temp_directory_path(ec) / leaf;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    return dir;
}

void test_add_find_duplicate() {
    VisusPackageCatalog cat;
    PT_ASSERT(cat.add(make("toon", VisusShaderRef::builtin("toon"))), "add toon");
    std::string err;
    PT_ASSERT(!cat.add(make("toon", VisusShaderRef::builtin()), {}, &err), "duplicate rejected");
    PT_ASSERT(err.find("duplicate") != std::string::npos, "duplicate message");
    PT_ASSERT(!cat.add(make("", VisusShaderRef::builtin()), {}, &err), "empty name rejected");
    PT_ASSERT(cat.add(make("outline", VisusShaderRef::builtin())), "add outline");

    PT_ASSERT(cat.contains("toon") && cat.contains("outline"), "contains");
    PT_ASSERT(cat.find("missing") == nullptr, "missing package");
    PT_ASSERT_OP(cat.size(), ==, size_t{2}, "size");
    const std::vector<std::string> names = cat.names();
    PT_ASSERT(names.size() == 2 && names[0] == "outline" && names[1] == "toon", "names sorted");
    PT_ASSERT(cat.remove("toon") && cat.size() == 1, "remove");
    PT_ASSERT(!cat.remove("toon"), "remove twice is false");
    cat.clear();
    PT_ASSERT_OP(cat.size(), ==, size_t{0}, "clear");
}

void test_load_directory() {
    const fs::path dir = temp_dir("pictor_shaderpkg_catalog");
    write_file(dir / "toon.shaderpkg.json",
               to_shader_package_json(make("toon", VisusShaderRef::stages(
                   "../shaders/toon.vert.spv", "../shaders/toon.frag.spv"))));
    // name 省略 → ファイル名が name になる。
    write_file(dir / "outline.shaderpkg.json",
               "{\n  \"version\": 1,\n  \"shader\": \"builtin:outline\"\n}\n");
    // 名前がファイル名と食い違う → warning (読み込み自体は通る)。
    write_file(dir / "rim.shaderpkg.json",
               to_shader_package_json(make("rim_light", VisusShaderRef::builtin())));
    // 拡張子違いは無視。
    write_file(dir / "notes.json", "{}");
    // 壊れた JSON は error になり、 他のファイルは読める。
    write_file(dir / "broken.shaderpkg.json", "{ this is not json");

    VisusPackageCatalog cat;
    std::vector<std::string> errors, warnings;
    const size_t loaded = cat.load_directory(dir.generic_string(), &errors, &warnings);
    PT_ASSERT_OP(loaded, ==, size_t{3}, "three packages loaded");
    PT_ASSERT(any_contains(errors, "broken.shaderpkg.json"), "broken file reported");
    PT_ASSERT(any_contains(warnings, "rim"), "stem mismatch warned");
    PT_ASSERT(cat.find("outline") != nullptr, "name defaults to the file stem");
    PT_ASSERT(cat.find("rim_light") != nullptr, "declared name wins over the stem");
    PT_ASSERT(cat.find("notes") == nullptr, "other json files ignored");

    const VisusShaderPackage* toon = cat.find("toon");
    PT_ASSERT(toon && toon->shader.kind == VisusShaderRef::Kind::STAGES, "stages round-trip");

    // stage パスはパッケージファイル起点で解決される。
    const std::string vert = cat.resolve_path("toon", toon->shader.vert);
    PT_ASSERT(vert.find("shaders/toon.vert.spv") != std::string::npos, "relative path resolved");
    PT_ASSERT(vert.find("..") == std::string::npos, "resolved path is normalized");

    const fs::path linked = dir / "linked.shaderpkg.json";
    std::error_code ec;
    fs::create_symlink(dir / "toon.shaderpkg.json", linked, ec);
    if (!ec) {
        std::string error;
        PT_ASSERT(!cat.load_file(linked.generic_string(), &error),
                  "direct package load refuses symbolic links");
        PT_ASSERT(error.find("symbolic link") != std::string::npos,
                  "direct package symlink refusal is explicit");
    }

    fs::remove_all(dir, ec);
}

void test_load_directory_missing() {
    VisusPackageCatalog cat;
    std::vector<std::string> errors;
    PT_ASSERT_OP(cat.load_directory("no/such/dir", &errors), ==, size_t{0}, "missing dir");
    PT_ASSERT(any_contains(errors, "not a directory"), "missing dir reported");
}

void test_resolve_path_edge_cases() {
    VisusPackageCatalog cat;
    cat.add(make("inline_pkg", VisusShaderRef::builtin()));   // source_path 無し
    PT_ASSERT(cat.resolve_path("inline_pkg", "a/b.spv") == "a/b.spv",
              "no source path -> relative path is returned as-is");
    PT_ASSERT(cat.resolve_path("inline_pkg", "").empty(), "empty relative path");
    PT_ASSERT(cat.resolve_path("missing", "a.spv") == "a.spv", "unknown package");

    constexpr char nul_data[] = "shader\0outside.spv";
    const std::string nul_path(nul_data, sizeof(nul_data) - 1);
    PT_ASSERT(cat.resolve_path("inline_pkg", nul_path).empty(), "embedded NUL fails closed");

    std::string err;
    PT_ASSERT(!cat.load_file(nul_path, &err), "embedded NUL load path rejected");
    PT_ASSERT(!cat.load_file("", &err), "empty load path rejected");
}

} // namespace

int main() {
    test_add_find_duplicate();
    test_load_directory();
    test_load_directory_missing();
    test_resolve_path_edge_cases();
    return report("unit_visus_package_catalog_test");
}
