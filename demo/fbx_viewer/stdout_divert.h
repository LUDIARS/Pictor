// Keeps stdout clean for machine output (`--raw-out -`, `--list-channels`):
// the original stdout descriptor is duplicated into a private stream, and
// the process-wide stdout is pointed at stderr so every existing log line
// lands on stderr (SPEC-PC-FBX-TRACK-PLAYBACK).
#pragma once

#include <cstdio>

namespace pictor_fbx_viewer {

/// Returns a stream on the original stdout (binary mode when `binary`), or
/// nullptr on failure. Call once, before machine output is written; the
/// caller owns the returned stream and closes it with std::fclose.
std::FILE* divert_stdout_to_stderr(bool binary);

} // namespace pictor_fbx_viewer
