#pragma once

#include "pictor/visus/visus.h"

#include <cstdint>
#include <string>
#include <vector>

namespace pictor {

// ============================================================
// IResourceLoader — ResourceRef → bytes 解決インターフェース
// ============================================================
// Pictor 本体は libcurl 等の HTTP 依存を持たない (上位非依存ルール、
// [[feedback_pictor_no_upper_dep]])。 ネットワーク取得は呼び出し側
// (ゲーム or Memoria 等) が自前で実装し注入する。
//
// 同梱されるのは local-only の `FileSystemResourceLoader` のみ。

class IResourceLoader {
public:
    virtual ~IResourceLoader() = default;

    /// ResourceRef を解決して bytes を返す。 失敗時は空 vector + error 文字列。
    /// `error` (non-null) には人間可読のエラーメッセージを入れる。
    virtual std::vector<uint8_t> fetch(const ResourceRef& ref,
                                       std::string*       error = nullptr) = 0;
};

// ============================================================
// FileSystemResourceLoader — local 専用 (Pictor 同梱の default)
// ============================================================
// root + ref.local_path で実ファイルを開く。 remote_url が指定されて
// いて fetch_policy が LOCAL_ONLY 以外でも、 このローダは無視して
// local しか試みない。 ネットワークが必要なら呼び出し側が別ローダを
// 注入する。

class FileSystemResourceLoader : public IResourceLoader {
public:
    explicit FileSystemResourceLoader(std::string root);

    std::vector<uint8_t> fetch(const ResourceRef& ref,
                               std::string*       error = nullptr) override;

    const std::string& root() const { return root_; }

private:
    std::string root_;
};

} // namespace pictor
