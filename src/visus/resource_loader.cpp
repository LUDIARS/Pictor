#include "pictor/visus/resource_loader.h"

#include <filesystem>
#include <fstream>
#include <ios>
#include <string>
#include <system_error>
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

    std::string full;
    bool absolute = false;
#ifdef _WIN32
    absolute = ref.local_path.size() >= 2 &&
               ((ref.local_path[1] == ':') ||
                (ref.local_path[0] == '/' || ref.local_path[0] == '\\'));
#else
    absolute = !ref.local_path.empty() && ref.local_path[0] == '/';
#endif

    if (root_.empty()) {
        // No sandbox configured: caller is trusted to supply an absolute or
        // already-validated path (legacy behaviour, kept intentionally).
        full = ref.local_path;
    } else {
        // A root_ sandbox is configured, so local_path is attacker-controllable
        // (visus JSON is supplied via CDN/UGC). Reject absolute paths and
        // confirm the resolved path stays inside root_ — closes ".." / absolute
        // traversal that would otherwise read arbitrary files.
        if (absolute) {
            return set_error("FileSystemResourceLoader: absolute path rejected");
        }
        namespace fs = std::filesystem;
        std::error_code ec;
        const fs::path root_canon = fs::weakly_canonical(fs::path(root_), ec);
        if (ec) return set_error("root canonicalization failed");
        const fs::path resolved =
            fs::weakly_canonical(root_canon / fs::path(ref.local_path), ec);
        if (ec) return set_error("path canonicalization failed");

        // resolved must be root_canon itself or a descendant of it.
        const auto rel = resolved.lexically_relative(root_canon);
        if (rel.empty() || *rel.begin() == "..") {
            return set_error("FileSystemResourceLoader: path escapes root");
        }
        full = resolved.string();
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
