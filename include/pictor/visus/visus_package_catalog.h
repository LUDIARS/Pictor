#pragma once

#include "pictor/visus/visus_shader_package.h"

#include <cstddef>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace pictor {

// ============================================================
// VisusPackageCatalog — name -> VisusShaderPackage (§2.5.1)
// ============================================================
// ディレクトリの `*.shaderpkg.json` を読む。 VisusCatalog と同じ方針:
// 同一性は name のみ、 handle は持たない、 symlink は拒否、 読込順は
// ファイル名でソートして決定的にする。
//
// shader stage のパス解決は**パッケージファイル起点**なので、 読込元
// (source_path) をエントリごとに覚えて `resolve_path` で使う。

struct VisusPackageEntry {
    VisusShaderPackage package;
    std::string        source_path;   // 読込元 (generic 形式の正規化パス)
};

class VisusPackageCatalog {
public:
    VisusPackageCatalog() = default;

    /// `dir` 直下の `*.shaderpkg.json` を全て読む (非再帰)。 戻り値は読めた件数。
    size_t load_directory(const std::string&        dir,
                          std::vector<std::string>* errors   = nullptr,
                          std::vector<std::string>* warnings = nullptr);

    /// 1 通常ファイルを読んで追加 (symlink は拒否)。name が空ならファイル名
    /// (".shaderpkg.json" を除く) を name にする。食い違うときは warnings。
    bool load_file(const std::string&        path,
                   std::string*              error    = nullptr,
                   std::vector<std::string>* warnings = nullptr);

    /// 組み立て済みのパッケージを追加。 name 重複は false (置換しない)。
    bool add(VisusShaderPackage package, std::string source_path = {},
             std::string* error = nullptr);

    bool remove(std::string_view name);
    void clear() { entries_.clear(); }

    const VisusShaderPackage* find(std::string_view name) const;
    const VisusPackageEntry*  entry(std::string_view name) const;
    bool contains(std::string_view name) const { return find(name) != nullptr; }

    size_t                   size() const { return entries_.size(); }
    std::vector<std::string> names() const;   // ソート済み

    /// `rel` を `package` の読込元ディレクトリ起点で解決した generic パス。
    /// 絶対パスは正規化のみ。 source_path が無ければ `rel` をそのまま返す。
    /// filesystem path として不正なら空文字。
    std::string resolve_path(std::string_view package, std::string_view rel) const;

private:
    std::map<std::string, VisusPackageEntry, std::less<>> entries_;
};

} // namespace pictor
