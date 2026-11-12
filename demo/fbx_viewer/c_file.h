// fopen for the fbx_viewer demo modules. MSVC deprecates fopen (C4996), which the /sdl of
// Lorica's baseline hardening makes an error, so Windows goes through fopen_s.
#pragma once

#include <cstdio>

namespace pictor_fbx_viewer {

/// The opened stream, or nullptr when the file cannot be opened.
inline std::FILE* open_c_file(const char* path, const char* mode) {
#if defined(_MSC_VER)
    std::FILE* file = nullptr;
    return fopen_s(&file, path, mode) == 0 ? file : nullptr;
#else
    return std::fopen(path, mode);
#endif
}

} // namespace pictor_fbx_viewer
