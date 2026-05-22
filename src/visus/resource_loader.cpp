#include "pictor/visus/resource_loader.h"

#include <fstream>
#include <ios>
#include <string>
#include <utility>
#include <vector>

namespace pictor {

FileSystemResourceLoader::FileSystemResourceLoader(std::string root)
    : root_(std::move(root)) {}

std::vector<uint8_t> FileSystemResourceLoader::fetch(const ResourceRef& ref,
                                                    std::string*       error)
{
    auto set_error = [&](const char* msg) {
        if (error) *error = msg;
        return std::vector<uint8_t>{};
    };

    if (ref.local_path.empty()) {
        // local 専用ローダなので remote_url 単独では解決不能。
        return set_error("FileSystemResourceLoader: local_path is empty");
    }

    // root + path を素直に結合 (絶対パスはそのまま使う想定で、 ここでは
    // root_ が空または末尾 / の正規化はしない — 呼び出し側の責任)。
    std::string full;
    bool absolute = false;
#ifdef _WIN32
    absolute = ref.local_path.size() >= 2 &&
               ((ref.local_path[1] == ':') ||
                (ref.local_path[0] == '/' || ref.local_path[0] == '\\'));
#else
    absolute = !ref.local_path.empty() && ref.local_path[0] == '/';
#endif
    if (absolute || root_.empty()) {
        full = ref.local_path;
    } else {
        full = root_;
        if (full.back() != '/' && full.back() != '\\') full.push_back('/');
        full += ref.local_path;
    }

    std::ifstream f(full, std::ios::binary | std::ios::ate);
    if (!f) return set_error("file open failed");

    const std::streamsize sz = f.tellg();
    if (sz < 0) return set_error("tellg failed");
    f.seekg(0, std::ios::beg);

    std::vector<uint8_t> buf(static_cast<size_t>(sz));
    if (sz > 0 && !f.read(reinterpret_cast<char*>(buf.data()), sz)) {
        return set_error("short read");
    }
    return buf;
}

} // namespace pictor
