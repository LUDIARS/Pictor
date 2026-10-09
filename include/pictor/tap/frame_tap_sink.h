#pragma once

/// フレームタップの配送先 — 1 フレーム 1 行の JSON を受け取る (spec/feature/frame-tap.md §2)。
///
/// 3 種 (stdout / ファイル追記 / コールバック) はどれも「1 行を外へ渡す」だけの
/// 同じ責務の実装違いなので 1 ファイルにまとめる。 行は改行を含まず、
/// 配送先が LF を 1 つ付ける。

#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace pictor {

class IFrameTapSink {
public:
    virtual ~IFrameTapSink() = default;

    /// 改行を含まない 1 行。 呼び出しは描画スレッドから同期で行われる。
    virtual void write_line(std::string_view line) = 0;
};

/// 標準出力へ JSON Lines。 フレームごとに flush する (パイプの読み手を待たせない)。
class StdoutFrameTapSink final : public IFrameTapSink {
public:
    void write_line(std::string_view line) override;
};

/// ファイルへバイナリ追記 (改行は LF 固定)。 所有したハンドルはデストラクタで閉じる。
class FileFrameTapSink final : public IFrameTapSink {
public:
    /// 開けなければ null と理由 (`error` non-null 時)。
    static std::unique_ptr<FileFrameTapSink> open(const std::string& path,
                                                  std::string*       error = nullptr);

    ~FileFrameTapSink() override;

    FileFrameTapSink(const FileFrameTapSink&) = delete;
    FileFrameTapSink& operator=(const FileFrameTapSink&) = delete;

    void write_line(std::string_view line) override;

private:
    explicit FileFrameTapSink(std::FILE* file) : file_(file) {}

    std::FILE* file_ = nullptr;
};

/// 呼び出し側のコールバックへ 1 行ずつ渡す (テストや埋め込みホスト用)。
class CallbackFrameTapSink final : public IFrameTapSink {
public:
    using Callback = std::function<void(std::string_view line)>;

    explicit CallbackFrameTapSink(Callback callback) : callback_(std::move(callback)) {}

    void write_line(std::string_view line) override;

private:
    Callback callback_;
};

} // namespace pictor
