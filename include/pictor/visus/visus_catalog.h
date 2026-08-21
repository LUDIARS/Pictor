#pragma once

#include "pictor/visus/visus.h"

#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace pictor {

// ============================================================
// VisusCatalog — name → VisusDesc の置き場 (handle 連番の VisusRegistry を置換)
// ============================================================
// 同一性は `name` のみ (`spec/feature/visus-v2-design.md` §3.2)。
// ディレクトリの `*.visus.json` を読み、 children の名前 / パス参照を
// 解決し、 循環参照と入れ子深さ (上限 kMaxChildDepth) を検査する。
//
// ResourceRef / handle の解決は持たない。 実行時資源は VisusRuntime
// (task 2) の責務。

struct VisusCatalogEntry {
    VisusDesc   desc;
    std::string source_path;   // 読込元 (generic 形式の正規化パス)。 add() 直指定なら空も可
};

class VisusCatalog {
public:
    static constexpr int kMaxChildDepth = 8;

    VisusCatalog() = default;

    /// `dir` 直下の `*.visus.json` を全て読む (非再帰)。 戻り値は読めた件数。
    /// 構文エラー / 重複 name は `errors` に積み、 そのファイルは飛ばす。
    /// 全ファイル読込後の未解決 child / 循環 / 深さ超過も `errors` に積む。
    size_t load_directory(const std::string&        dir,
                          std::vector<std::string>* errors   = nullptr,
                          std::vector<std::string>* warnings = nullptr);

    /// 1 ファイルを読んで追加。 name が空ならファイル名 (".visus.json" を除く)
    /// を name にする。 ファイル名と name が食い違うときは warnings。
    bool load_file(const std::string&        path,
                   std::string*              error    = nullptr,
                   std::vector<std::string>* warnings = nullptr);

    /// 組み立て済みの desc を追加。 name 重複は false (置換しない)。
    bool add(VisusDesc desc, std::string source_path = {}, std::string* error = nullptr);

    bool remove(std::string_view name);
    void clear() { entries_.clear(); }

    const VisusDesc*         find(std::string_view name) const;
    const VisusCatalogEntry* entry(std::string_view name) const;
    bool                     contains(std::string_view name) const { return find(name) != nullptr; }

    size_t                   size() const { return entries_.size(); }
    std::vector<std::string> names() const;   // ソート済み

    /// `ref` を親 desc の文脈で解決する。 名前参照はそのまま name で引く。
    /// パス参照 (".visus.json" / '/' を含む) は親の source_path のディレクトリ
    /// 起点で正規化し、 同じ source_path を持つエントリを返す。 無ければ
    /// nullptr + `error`。
    const VisusDesc* resolve_child(const VisusDesc&     parent,
                                   const VisusChildRef& ref,
                                   std::string*         error = nullptr) const;

    /// `rel` を desc の visus ファイル起点で解決した generic パス。 絶対パスは
    /// 正規化のみ。 desc に source_path が無ければ `rel` をそのまま返す。
    ///
    /// load_directory / load_file で読んだエントリの source_path は絶対化されて
    /// いるため、 戻り値も絶対パスになる。 root を設定した
    /// `FileSystemResourceLoader` は絶対パスを拒む (sandbox) ので、 そのローダへ
    /// 渡す側が root 相対へ落とす責務を持つ (task 2 の VisusRuntime)。
    std::string resolve_path(const VisusDesc& desc, std::string_view rel) const;

    /// 全エントリの children を検査: 未解決参照 / 循環 / 深さ超過を
    /// 人間可読メッセージで返す。 空なら健全。
    std::vector<std::string> validate() const;

    /// 1 エントリを root として children を検査 (validate の部品)。
    void validate_tree(std::string_view root_name, std::vector<std::string>& out) const;

private:
    const VisusCatalogEntry* entry_by_source_(std::string_view source_path) const;
    void validate_node_(const VisusDesc& node, int depth,
                        std::vector<std::string>& stack,
                        std::map<std::string, uint16_t, std::less<>>& completed_depths,
                        std::vector<std::string>& out) const;

    std::map<std::string, VisusCatalogEntry, std::less<>> entries_;
};

} // namespace pictor
