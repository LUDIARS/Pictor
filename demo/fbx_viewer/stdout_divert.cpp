#include "stdout_divert.h"

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#else
#include <unistd.h>
#endif

namespace pictor_fbx_viewer {

/// @implements SPEC-PC-FBX-TRACK-PLAYBACK
std::FILE* divert_stdout_to_stderr(bool binary) {
    std::fflush(stdout);
#ifdef _WIN32
    const int machine_fd = _dup(_fileno(stdout));
    if (machine_fd < 0) return nullptr;
    if (binary) _setmode(machine_fd, _O_BINARY);
    if (_dup2(_fileno(stderr), _fileno(stdout)) != 0) { _close(machine_fd); return nullptr; }
    std::FILE* f = _fdopen(machine_fd, binary ? "wb" : "w");
    if (!f) _close(machine_fd);
#else
    (void)binary;  // POSIX streams do not translate line endings.
    const int machine_fd = dup(fileno(stdout));
    if (machine_fd < 0) return nullptr;
    if (dup2(fileno(stderr), fileno(stdout)) < 0) { close(machine_fd); return nullptr; }
    std::FILE* f = fdopen(machine_fd, "w");
    if (!f) close(machine_fd);
#endif
    return f;
}

} // namespace pictor_fbx_viewer
