#include "pictor/visus/resource_loader.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <ios>
#include <limits>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <climits>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace pictor {

namespace {

namespace fs = std::filesystem;

bool fail(std::string* error, const char* message) {
    if (error) *error = message;
    return false;
}

bool normalized_relative_path(std::string_view input, fs::path& out,
                              std::string* error) {
#ifdef _WIN32
    // ':' in a relative Win32 path selects an NTFS alternate data stream.
    // Asset paths name ordinary files only; allowing ADS could expose metadata
    // attached to an otherwise permitted file.
    if (input.find(':') != std::string_view::npos)
        return fail(error, "FileSystemResourceLoader: alternate data stream rejected");
#endif
    try {
        const fs::path raw{std::string(input)};
        if (raw.is_absolute() || raw.has_root_name() || raw.has_root_directory())
            return fail(error, "FileSystemResourceLoader: absolute path rejected");

        out = raw.lexically_normal();
        bool has_component = false;
        for (const fs::path& component : out) {
            if (component == ".") continue;
            if (component == "..")
                return fail(error, "FileSystemResourceLoader: path escapes root");
            has_component = true;
        }
        if (!has_component)
            return fail(error, "FileSystemResourceLoader: path is empty");
        return true;
    } catch (const fs::filesystem_error&) {
        return fail(error, "FileSystemResourceLoader: invalid filesystem path");
    }
}

bool read_unrestricted(const std::string& path, uint64_t max_file_bytes,
                       std::vector<uint8_t>& out, std::string* error) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) return fail(error, "file open failed");

    const std::streamsize size = file.tellg();
    if (size < 0) return fail(error, "tellg failed");
    const auto unsigned_size = static_cast<uintmax_t>(size);
    if (unsigned_size > max_file_bytes)
        return fail(error, "file exceeds configured size limit");
    if (unsigned_size > std::numeric_limits<size_t>::max())
        return fail(error, "file is too large for this process");

    out.assign(static_cast<size_t>(size), uint8_t{0});
    file.seekg(0, std::ios::beg);
    if (size > 0 && !file.read(reinterpret_cast<char*>(out.data()), size)) {
        out.clear();
        return fail(error, "short read");
    }
    return true;
}

#ifdef _WIN32

class ScopedHandle {
public:
    explicit ScopedHandle(HANDLE handle = INVALID_HANDLE_VALUE) : handle_(handle) {}
    ~ScopedHandle() { if (valid()) CloseHandle(handle_); }

    ScopedHandle(const ScopedHandle&) = delete;
    ScopedHandle& operator=(const ScopedHandle&) = delete;

    bool valid() const { return handle_ != INVALID_HANDLE_VALUE && handle_ != nullptr; }
    HANDLE get() const { return handle_; }

private:
    HANDLE handle_;
};

bool final_path(HANDLE handle, std::wstring& out) {
    constexpr DWORD flags = FILE_NAME_NORMALIZED | VOLUME_NAME_DOS;
    const DWORD required = GetFinalPathNameByHandleW(handle, nullptr, 0, flags);
    if (required == 0) return false;
    out.assign(required, L'\0');
    const DWORD written = GetFinalPathNameByHandleW(handle, out.data(), required, flags);
    if (written == 0 || written >= required) return false;
    out.resize(written);
    return true;
}

bool path_is_beneath(std::wstring_view root, std::wstring_view path) {
    while (root.size() > 1 && (root.back() == L'\\' || root.back() == L'/'))
        root.remove_suffix(1);
    if (path.size() <= root.size()) return false;
    if (CompareStringOrdinal(path.data(), static_cast<int>(root.size()),
                             root.data(), static_cast<int>(root.size()), TRUE) != CSTR_EQUAL)
        return false;
    return path[root.size()] == L'\\' || path[root.size()] == L'/';
}

bool read_beneath_root(const std::string& root, const fs::path& relative,
                       uint64_t max_file_bytes, std::vector<uint8_t>& out,
                       std::string* error) {
    std::error_code ec;
    const fs::path canonical_root = fs::canonical(fs::path(root), ec);
    if (ec || !fs::is_directory(canonical_root, ec) || ec)
        return fail(error, "root canonicalization failed");

    constexpr DWORD share = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
    ScopedHandle root_handle(CreateFileW(
        canonical_root.c_str(), FILE_READ_ATTRIBUTES, share, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (!root_handle.valid()) return fail(error, "root open failed");

    BY_HANDLE_FILE_INFORMATION root_info{};
    if (!GetFileInformationByHandle(root_handle.get(), &root_info) ||
        (root_info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
        return fail(error, "root is not a directory");
    if ((root_info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
        return fail(error, "root symbolic link refused");

    const fs::path candidate = canonical_root / relative;
    ScopedHandle file_handle(CreateFileW(
        candidate.c_str(), GENERIC_READ, share, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (!file_handle.valid()) return fail(error, "file open failed");

    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(file_handle.get(), &info))
        return fail(error, "file information query failed");
    if ((info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
        return fail(error, "FileSystemResourceLoader: symbolic link refused");
    if ((info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 ||
        GetFileType(file_handle.get()) != FILE_TYPE_DISK)
        return fail(error, "FileSystemResourceLoader: not a regular file");

    // Validate the objects actually opened, rather than a pathname checked
    // before open. An attacker cannot win a symlink/junction swap between a
    // canonicalization check and the read.
    std::wstring opened_root;
    std::wstring opened_file;
    if (!final_path(root_handle.get(), opened_root) ||
        !final_path(file_handle.get(), opened_file))
        return fail(error, "opened path query failed");
    if (!path_is_beneath(opened_root, opened_file))
        return fail(error, "FileSystemResourceLoader: path escapes root");

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file_handle.get(), &size) || size.QuadPart < 0)
        return fail(error, "file size query failed");
    const uint64_t unsigned_size = static_cast<uint64_t>(size.QuadPart);
    if (unsigned_size > max_file_bytes)
        return fail(error, "file exceeds configured size limit");
    if (unsigned_size > std::numeric_limits<size_t>::max())
        return fail(error, "file is too large for this process");

    out.assign(static_cast<size_t>(unsigned_size), uint8_t{0});
    size_t offset = 0;
    while (offset < out.size()) {
        const DWORD requested = static_cast<DWORD>(std::min<size_t>(
            out.size() - offset, std::numeric_limits<DWORD>::max()));
        DWORD read = 0;
        if (!ReadFile(file_handle.get(), out.data() + offset, requested, &read, nullptr) ||
            read == 0) {
            out.clear();
            return fail(error, "short read");
        }
        offset += read;
    }
    return true;
}

#else

class ScopedFd {
public:
    explicit ScopedFd(int fd = -1) : fd_(fd) {}
    ~ScopedFd() { if (fd_ >= 0) ::close(fd_); }

    ScopedFd(const ScopedFd&) = delete;
    ScopedFd& operator=(const ScopedFd&) = delete;

    ScopedFd(ScopedFd&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}
    ScopedFd& operator=(ScopedFd&& other) noexcept {
        if (this != &other) {
            if (fd_ >= 0) ::close(fd_);
            fd_ = std::exchange(other.fd_, -1);
        }
        return *this;
    }

    bool valid() const { return fd_ >= 0; }
    int get() const { return fd_; }

private:
    int fd_;
};

int close_on_exec_flag() {
#ifdef O_CLOEXEC
    return O_CLOEXEC;
#else
    return 0;
#endif
}

bool read_beneath_root(const std::string& root, const fs::path& relative,
                       uint64_t max_file_bytes, std::vector<uint8_t>& out,
                       std::string* error) {
#ifndef O_NOFOLLOW
    (void)root; (void)relative; (void)max_file_bytes; (void)out;
    return fail(error, "configured-root loading requires O_NOFOLLOW support");
#else
    std::error_code ec;
    const fs::path canonical_root = fs::canonical(fs::path(root), ec);
    if (ec || !fs::is_directory(canonical_root, ec) || ec)
        return fail(error, "root canonicalization failed");

    ScopedFd current(::open(canonical_root.c_str(),
                            O_RDONLY | O_DIRECTORY | close_on_exec_flag() | O_NOFOLLOW));
    if (!current.valid()) return fail(error, "root open failed");

    std::vector<fs::path> components;
    for (const fs::path& component : relative) {
        if (component != ".") components.push_back(component);
    }
    for (size_t i = 0; i < components.size(); ++i) {
        const bool last = i + 1 == components.size();
        int flags = O_RDONLY | close_on_exec_flag() | O_NOFOLLOW;
        if (!last) flags |= O_DIRECTORY;
        ScopedFd next(::openat(current.get(), components[i].c_str(), flags));
        if (!next.valid())
            return fail(error, errno == ELOOP
                ? "FileSystemResourceLoader: symbolic link refused"
                : "file open failed");
        current = std::move(next);
    }

    struct stat status{};
    if (::fstat(current.get(), &status) != 0)
        return fail(error, "file information query failed");
    if (!S_ISREG(status.st_mode))
        return fail(error, "FileSystemResourceLoader: not a regular file");
    if (status.st_size < 0) return fail(error, "file size query failed");
    const uint64_t unsigned_size = static_cast<uint64_t>(status.st_size);
    if (unsigned_size > max_file_bytes)
        return fail(error, "file exceeds configured size limit");
    if (unsigned_size > std::numeric_limits<size_t>::max())
        return fail(error, "file is too large for this process");

    out.assign(static_cast<size_t>(unsigned_size), uint8_t{0});
    size_t offset = 0;
    while (offset < out.size()) {
        const size_t requested = std::min<size_t>(out.size() - offset, SSIZE_MAX);
        const ssize_t count = ::read(current.get(), out.data() + offset, requested);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) {
            out.clear();
            return fail(error, "short read");
        }
        offset += static_cast<size_t>(count);
    }
    return true;
#endif
}

#endif

} // namespace

FileSystemResourceLoader::FileSystemResourceLoader(std::string root,
                                                   uint64_t    max_file_bytes)
    : root_(std::move(root)), max_file_bytes_(max_file_bytes) {}

std::vector<uint8_t> FileSystemResourceLoader::fetch(std::string_view path,
                                                    std::string*     error) {
    if (error) error->clear();
    if (path.empty()) {
        if (error) *error = "FileSystemResourceLoader: path is empty";
        return {};
    }
    if (path.find('\0') != std::string_view::npos) {
        if (error) *error = "FileSystemResourceLoader: path contains NUL";
        return {};
    }

    std::vector<uint8_t> bytes;
    if (root_.empty()) {
        if (!read_unrestricted(std::string(path), max_file_bytes_, bytes, error)) return {};
        return bytes;
    }

    fs::path relative;
    if (!normalized_relative_path(path, relative, error)) return {};
    if (!read_beneath_root(root_, relative, max_file_bytes_, bytes, error)) return {};
    return bytes;
}

} // namespace pictor
