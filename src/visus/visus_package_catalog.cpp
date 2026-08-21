#include "pictor/visus/visus_package_catalog.h"

#include "pictor/visus/visus_package_serializer.h"
#include "visus_json.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <system_error>

namespace pictor {

namespace fs = std::filesystem;

namespace {

constexpr std::string_view kSuffix = ".shaderpkg.json";

bool ends_with(std::string_view s, std::string_view suffix) {
    return s.size() >= suffix.size() && s.substr(s.size() - suffix.size()) == suffix;
}

std::string normalize(const fs::path& p) {
    std::error_code ec;
    fs::path abs = fs::absolute(p, ec);
    if (ec) abs = p;
    return abs.lexically_normal().generic_string();
}

std::string stem_name(const fs::path& p) {
    std::string fn = p.filename().generic_string();
    if (ends_with(fn, kSuffix)) fn.resize(fn.size() - kSuffix.size());
    return fn;
}

void push(std::vector<std::string>* v, std::string msg) {
    if (v) v->push_back(std::move(msg));
}

} // namespace

// ---- loading -----------------------------------------------------------------

size_t VisusPackageCatalog::load_directory(const std::string&        dir,
                                           std::vector<std::string>* errors,
                                           std::vector<std::string>* warnings) {
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) {
        push(errors, "shader package catalog: not a directory: " + dir);
        return 0;
    }
    // 決定的な順序で読む (ディレクトリ列挙順は OS 依存)。
    std::vector<fs::path> files;
    std::vector<fs::path> rejected_symlinks;
    fs::directory_iterator it(dir, ec);
    const fs::directory_iterator end;
    while (it != end && !ec) {
        const bool has_suffix = ends_with(it->path().filename().generic_string(), kSuffix);
        std::error_code file_ec;
        if (has_suffix && it->is_symlink(file_ec)) {
            rejected_symlinks.push_back(it->path());
        } else if (has_suffix && !file_ec && it->is_regular_file(file_ec)) {
            files.push_back(it->path());
        }
        it.increment(ec);
    }
    if (ec) push(errors, "shader package catalog: directory enumeration failed: " + dir);
    std::sort(files.begin(), files.end());
    std::sort(rejected_symlinks.begin(), rejected_symlinks.end());
    for (const fs::path& path : rejected_symlinks)
        push(errors, path.generic_string() + ": symbolic link refused");

    size_t loaded = 0;
    for (const fs::path& f : files) {
        std::string err;
        if (load_file(f.generic_string(), &err, warnings)) {
            ++loaded;
        } else {
            push(errors, f.generic_string() + ": " + err);
        }
    }
    return loaded;
}

bool VisusPackageCatalog::load_file(const std::string&        path,
                                    std::string*              error,
                                    std::vector<std::string>* warnings) {
    std::string json;
    if (!visus_json::read_bounded_file(path, json, error)) return false;

    VisusShaderPackage pkg;
    std::vector<std::string> local_warnings;
    if (!from_shader_package_json(json, pkg, error, &local_warnings)) return false;
    for (std::string& w : local_warnings) push(warnings, path + ": " + w);

    const std::string stem = stem_name(path);
    if (pkg.name.empty()) {
        pkg.name = stem;
    } else if (pkg.name != stem) {
        push(warnings, path + ": name is not the file stem: " + pkg.name + " / " + stem);
    }
    return add(std::move(pkg), normalize(path), error);
}

bool VisusPackageCatalog::add(VisusShaderPackage package, std::string source_path,
                              std::string* error) {
    if (package.name.empty()) {
        if (error) *error = "shader package name is empty";
        return false;
    }
    if (entries_.find(package.name) != entries_.end()) {
        if (error) *error = "duplicate shader package name: " + package.name;
        return false;
    }
    if (!source_path.empty()) {
        source_path = normalize(source_path);
        // 同じファイルを別 name で二重登録させない (VisusCatalog と同方針)。
        for (const auto& [_, e] : entries_) {
            if (!e.source_path.empty() && e.source_path == source_path) {
                if (error) *error = "duplicate shader package source path: " + source_path;
                return false;
            }
        }
    }
    std::string key = package.name;
    entries_.emplace(std::move(key),
                     VisusPackageEntry{std::move(package), std::move(source_path)});
    return true;
}

bool VisusPackageCatalog::remove(std::string_view name) {
    auto it = entries_.find(name);
    if (it == entries_.end()) return false;
    entries_.erase(it);
    return true;
}

// ---- lookup ------------------------------------------------------------------

const VisusPackageEntry* VisusPackageCatalog::entry(std::string_view name) const {
    auto it = entries_.find(name);
    return it == entries_.end() ? nullptr : &it->second;
}

const VisusShaderPackage* VisusPackageCatalog::find(std::string_view name) const {
    const VisusPackageEntry* e = entry(name);
    return e ? &e->package : nullptr;
}

std::vector<std::string> VisusPackageCatalog::names() const {
    std::vector<std::string> out;
    out.reserve(entries_.size());
    for (const auto& [k, _] : entries_) out.push_back(k);
    return out;   // std::map なのでソート済み
}

std::string VisusPackageCatalog::resolve_path(std::string_view package,
                                              std::string_view rel) const {
    if (rel.empty() || rel.find(char{0}) != std::string_view::npos) return {};
    try {
        const fs::path rp{std::string(rel)};
        if (rp.is_absolute()) return rp.lexically_normal().generic_string();
        const VisusPackageEntry* e = entry(package);
        if (!e || e->source_path.empty()) return std::string(rel);
        return (fs::path(e->source_path).parent_path() / rp).lexically_normal().generic_string();
    } catch (const fs::filesystem_error&) {
        // Untrusted JSON may not be representable as a native filesystem path.
        return {};
    }
}

} // namespace pictor
