#include "pictor/tap/frame_tap_sink.h"

#include <cstdio>

namespace pictor {

namespace {

void write_line_to(std::FILE* file, std::string_view line) {
    if (!line.empty()) std::fwrite(line.data(), 1, line.size(), file);
    std::fputc('\n', file);
    // 読み手 (パイプ / tail) をフレーム単位で進めるため行ごとに flush する。
    std::fflush(file);
}

} // namespace

void StdoutFrameTapSink::write_line(std::string_view line) {
    write_line_to(stdout, line);
}

std::unique_ptr<FileFrameTapSink> FileFrameTapSink::open(const std::string& path,
                                                         std::string*       error) {
    if (path.empty()) {
        if (error) *error = "frame tap output path is empty";
        return nullptr;
    }
    // バイナリ追記: Windows のテキストモードで LF が CRLF に化けないようにする。
#ifdef _MSC_VER
    std::FILE* file = nullptr;
    if (fopen_s(&file, path.c_str(), "ab") != 0) file = nullptr;
#else
    std::FILE* file = std::fopen(path.c_str(), "ab");
#endif
    if (!file) {
        if (error) *error = "cannot open frame tap output for append: " + path;
        return nullptr;
    }
    return std::unique_ptr<FileFrameTapSink>(new FileFrameTapSink(file));
}

FileFrameTapSink::~FileFrameTapSink() {
    if (file_) std::fclose(file_);
}

void FileFrameTapSink::write_line(std::string_view line) {
    write_line_to(file_, line);
}

void CallbackFrameTapSink::write_line(std::string_view line) {
    if (callback_) callback_(line);
}

} // namespace pictor
