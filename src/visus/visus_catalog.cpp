#include "pictor/visus/visus_catalog.h"

#include "pictor/visus/visus_serializer.h"
#include "visus_json.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <system_error>

namespace pictor {

namespace fs = std::filesystem;

namespace {

constexpr std::string_view kSuffix = ".visus.json";

bool ends_with(std::string_view s, std::string_view suffix) {
    return s.size() >= suffix.size() &&
           s.substr(s.size() - suffix.size()) == suffix;
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

size_t VisusCatalog::load_directory(const std::string&        dir,
                                    std::vector<std::string>* errors,
                                    std::vector<std::string>* warnings) {
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) {
        push(errors, "visus catalog: not a directory: " + dir);
        return 0;
    }
    // 決定的な順序で読む (ディレクトリ列挙順は OS 依存)。
    std::vector<fs::path> files;
    fs::directory_iterator it(dir, ec);
    const fs::directory_iterator end;
    while (it != end && !ec) {
        std::error_code file_ec;
        if (it->is_regular_file(file_ec) &&
            ends_with(it->path().filename().generic_string(), kSuffix)) {
            files.push_back(it->path());
        }
        it.increment(ec);
    }
    if (ec) push(errors, "visus catalog: directory enumeration failed: " + dir);
    std::sort(files.begin(), files.end());

    size_t loaded = 0;
    for (const fs::path& f : files) {
        std::string err;
        if (load_file(f.generic_string(), &err, warnings)) {
            ++loaded;
        } else {
            push(errors, f.generic_string() + ": " + err);
        }
    }
    for (std::string& issue : validate()) push(errors, std::move(issue));
    return loaded;
}

bool VisusCatalog::load_file(const std::string&        path,
                             std::string*              error,
                             std::vector<std::string>* warnings) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) {
        if (error) *error = "open failed";
        return false;
    }
    const std::streamsize size = in.tellg();
    if (size < 0) {
        if (error) *error = "size query failed";
        return false;
    }
    if (size > static_cast<std::streamsize>(visus_json::kMaxInputBytes)) {
        if (error) *error = "visus json is too large";
        return false;
    }
    std::string json(static_cast<size_t>(size), '\0');
    in.seekg(0, std::ios::beg);
    if (size > 0 && !in.read(json.data(), size)) {
        if (error) *error = "read failed";
        return false;
    }

    VisusDesc desc;
    std::vector<std::string> local_warnings;
    if (!from_visus_json(json, desc, error, &local_warnings)) return false;
    for (std::string& w : local_warnings) push(warnings, path + ": " + w);

    const std::string stem = stem_name(path);
    if (desc.name.empty()) {
        desc.name = stem;
    } else if (desc.name != stem) {
        push(warnings, path + ": name '" + desc.name + "' differs from file stem '" + stem + "'");
    }
    return add(std::move(desc), normalize(path), error);
}

bool VisusCatalog::add(VisusDesc desc, std::string source_path, std::string* error) {
    if (desc.name.empty()) {
        if (error) *error = "visus name is empty";
        return false;
    }
    if (entries_.find(desc.name) != entries_.end()) {
        if (error) *error = "duplicate visus name: " + desc.name;
        return false;
    }
    if (!source_path.empty()) {
        source_path = normalize(source_path);
        if (entry_by_source_(source_path)) {
            if (error) *error = "duplicate visus source path: " + source_path;
            return false;
        }
    }
    std::string key = desc.name;
    entries_.emplace(std::move(key), VisusCatalogEntry{std::move(desc), std::move(source_path)});
    return true;
}

bool VisusCatalog::remove(std::string_view name) {
    auto it = entries_.find(name);
    if (it == entries_.end()) return false;
    entries_.erase(it);
    return true;
}

// ---- lookup ------------------------------------------------------------------

const VisusCatalogEntry* VisusCatalog::entry(std::string_view name) const {
    auto it = entries_.find(name);
    return it == entries_.end() ? nullptr : &it->second;
}

const VisusDesc* VisusCatalog::find(std::string_view name) const {
    const VisusCatalogEntry* e = entry(name);
    return e ? &e->desc : nullptr;
}

std::vector<std::string> VisusCatalog::names() const {
    std::vector<std::string> out;
    out.reserve(entries_.size());
    for (const auto& [k, _] : entries_) out.push_back(k);
    return out;   // std::map なのでソート済み
}

const VisusCatalogEntry* VisusCatalog::entry_by_source_(std::string_view source_path) const {
    for (const auto& [_, e] : entries_) {
        if (!e.source_path.empty() && e.source_path == source_path) return &e;
    }
    return nullptr;
}

std::string VisusCatalog::resolve_path(const VisusDesc& desc, std::string_view rel) const {
    const fs::path rp{std::string(rel)};
    if (rp.is_absolute()) return rp.lexically_normal().generic_string();
    const VisusCatalogEntry* e = entry(desc.name);
    if (!e || e->source_path.empty()) return std::string(rel);
    return (fs::path(e->source_path).parent_path() / rp).lexically_normal().generic_string();
}

const VisusDesc* VisusCatalog::resolve_child(const VisusDesc&     parent,
                                             const VisusChildRef& ref,
                                             std::string*         error) const {
    if (ref.visus.empty()) {
        if (error) *error = "child reference is empty";
        return nullptr;
    }
    if (!ref.is_path_ref()) {
        const VisusDesc* d = find(ref.visus);
        if (!d && error) *error = "child visus not found: " + ref.visus;
        return d;
    }
    const std::string full = resolve_path(parent, ref.visus);
    const VisusCatalogEntry* e = entry_by_source_(full);
    if (!e) {
        // add() 直指定で親の source_path が無い場合だけ stem fallback を
        // 許す。 Loaded parents must resolve the exact requested source path;
        // otherwise a same-named file elsewhere can mask a broken reference.
        const VisusCatalogEntry* parent_entry = entry(parent.name);
        if (!parent_entry || parent_entry->source_path.empty()) {
            const VisusDesc* by_stem = find(stem_name(full));
            if (by_stem) return by_stem;
        }
        if (error) *error = "child visus file not loaded: " + full;
        return nullptr;
    }
    return &e->desc;
}

// ---- validation --------------------------------------------------------------

void VisusCatalog::validate_node_(const VisusDesc&          node,
                                  int                       depth,
                                  std::vector<std::string>& stack,
                                  std::map<std::string, uint16_t, std::less<>>& completed_depths,
                                  std::vector<std::string>& out) const {
    for (const std::string& s : stack) {
        if (s == node.name) {
            std::string msg = "visus cycle: ";
            for (const std::string& t : stack) msg += t + " -> ";
            msg += node.name;
            out.push_back(std::move(msg));
            return;
        }
    }

    // A catalog is a graph, not necessarily a tree: the same child definition
    // may be instantiated many times. Validate each (name, depth) state once so
    // repeated/shared subtrees cannot make validation exponential. Depth is
    // bounded to kMaxChildDepth + 1, so a uint16_t is sufficient as a bitset.
    const int bounded_depth = std::min(depth, kMaxChildDepth + 1);
    const uint16_t depth_bit = static_cast<uint16_t>(uint16_t{1} << bounded_depth);
    uint16_t& completed = completed_depths[node.name];
    if ((completed & depth_bit) != 0) return;

    if (depth > kMaxChildDepth) {
        out.push_back("visus nesting too deep (>" + std::to_string(kMaxChildDepth) +
                      ") at: " + node.name);
        completed = static_cast<uint16_t>(completed | depth_bit);
        return;
    }
    stack.push_back(node.name);
    for (const VisusChildRef& c : node.children) {
        std::string err;
        const VisusDesc* child = resolve_child(node, c, &err);
        if (!child) {
            out.push_back(node.name + ": " + err);
            continue;
        }
        validate_node_(*child, depth + 1, stack, completed_depths, out);
    }
    stack.pop_back();
    completed = static_cast<uint16_t>(completed | depth_bit);
}

void VisusCatalog::validate_tree(std::string_view root_name, std::vector<std::string>& out) const {
    const VisusDesc* root = find(root_name);
    if (!root) {
        out.push_back("visus not found: " + std::string(root_name));
        return;
    }
    std::vector<std::string> stack;
    std::map<std::string, uint16_t, std::less<>> completed_depths;
    validate_node_(*root, 0, stack, completed_depths, out);
}

std::vector<std::string> VisusCatalog::validate() const {
    std::vector<std::string> out;
    std::vector<std::string> stack;
    std::map<std::string, uint16_t, std::less<>> completed_depths;
    for (const auto& [_, entry] : entries_) {
        stack.clear();
        validate_node_(entry.desc, 0, stack, completed_depths, out);
    }
    // 同じ循環は複数 root から検出されるので重複を畳む。
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

} // namespace pictor
