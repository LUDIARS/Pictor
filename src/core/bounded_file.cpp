#include "pictor/core/bounded_file.h"

#include <lorica/checked_integer.hpp>

#include <fstream>
#include <ios>

namespace pictor {

namespace {

// Reads path into out when its size is known and at most max_bytes; nothing is
// allocated for a file that fails either check.
template <class Buffer>
FileReadStatus read_into(const std::string& path, std::size_t max_bytes, Buffer& out) {
    out.clear();
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) return FileReadStatus::OPEN_FAILED;
    // tellg() is -1 on failure: narrow rejects it (and any size size_t cannot hold)
    // before the value can reach an allocation.
    const auto size = lorica::narrow<std::size_t>(static_cast<std::streamoff>(file.tellg()));
    if (!size) return FileReadStatus::SIZE_UNKNOWN;
    if (*size > max_bytes) return FileReadStatus::TOO_LARGE;
    out.resize(*size);
    file.seekg(0, std::ios::beg);
    if (*size > 0 &&
        !file.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(*size))) {
        out.clear();
        return FileReadStatus::READ_FAILED;
    }
    return FileReadStatus::OK;
}

} // namespace

const char* file_read_status_name(FileReadStatus status) noexcept {
    switch (status) {
        case FileReadStatus::OK:           return "ok";
        case FileReadStatus::OPEN_FAILED:  return "open_failed";
        case FileReadStatus::SIZE_UNKNOWN: return "size_unknown";
        case FileReadStatus::TOO_LARGE:    return "too_large";
        case FileReadStatus::READ_FAILED:  return "read_failed";
    }
    return "unknown";
}

FileReadStatus read_file_bounded(const std::string& path, std::size_t max_bytes,
                                 std::vector<uint8_t>& out) {
    return read_into(path, max_bytes, out);
}

FileReadStatus read_text_file_bounded(const std::string& path, std::size_t max_bytes,
                                      std::string& out) {
    return read_into(path, max_bytes, out);
}

} // namespace pictor
