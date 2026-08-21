#pragma once

// パス文字列しか扱わないので Visus 型には依存しない。

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace pictor {

// ============================================================
// IResourceLoader — パス → bytes 解決インターフェース
// ============================================================
// Visus v2 の資源参照はすべてパス文字列 (`asset` / shader stage /
// `texture.*`、 `spec/feature/visus-v2-design.md` §2.1)。 Pictor 本体は
// libcurl 等の HTTP 依存を持たない (上位非依存ルール、
// [[feedback_pictor_no_upper_dep]])。 remote 取得が要るホストはパスを
// URL に写像するローダを自前で実装し注入する。
//
// 同梱されるのは local-only の `FileSystemResourceLoader` のみ。

class IResourceLoader {
public:
    virtual ~IResourceLoader() = default;

    /// `path` を解決して bytes を返す。 失敗時は空 vector + error 文字列。
    /// `error` (non-null) には人間可読のエラーメッセージを入れる。
    virtual std::vector<uint8_t> fetch(std::string_view path,
                                       std::string*     error = nullptr) = 0;
};

// ============================================================
// FileSystemResourceLoader — local 専用 (Pictor 同梱の default)
// ============================================================
// root + path で実ファイルを開く。 root が設定されているときは絶対パスと
// root 外への脱出を拒否し、OS の handle / directory fd を基準に読むことで
// 検証後の symlink / junction 差し替えも拒否する (visus JSON は UGC / CDN
// 由来になり得る)。
// 読込前に `max_file_bytes` (既定 512 MiB) も検査し、巨大入力による
// 無制限 allocation を避ける。
//
// **root が空文字のときは containment を一切かけない** (size limit だけが
// 効く)。 パスをプロセス権限そのままで開くので、 信頼できない visus JSON
// (UGC / CDN 由来) を扱うホストは必ず root を設定すること。

class FileSystemResourceLoader : public IResourceLoader {
public:
    static constexpr uint64_t kDefaultMaxFileBytes = uint64_t{512} * 1024 * 1024;

    explicit FileSystemResourceLoader(
        std::string root,
        uint64_t    max_file_bytes = kDefaultMaxFileBytes);

    std::vector<uint8_t> fetch(std::string_view path,
                               std::string*     error = nullptr) override;

    const std::string& root() const { return root_; }

private:
    std::string root_;
    uint64_t    max_file_bytes_;
};

} // namespace pictor
