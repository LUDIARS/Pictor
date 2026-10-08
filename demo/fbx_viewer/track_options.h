// Command-line options of SPEC-PC-FBX-TRACK-PLAYBACK: track playback, raw
// frame output, fixed view (size / camera / fov / clear) and the headless
// channel listing. Parsed here so the viewer's main only wires them.
#pragma once

#include <cstdint>
#include <string>

namespace pictor_fbx_viewer {

struct TrackOptions {
    std::string track_path;          // --track <file.csv>
    uint32_t    track_fps    = 30;   // --track-fps <1..120>
    std::string raw_out;             // --raw-out <path|->
    bool        has_size     = false;
    uint32_t    width        = 0;    // --size WxH
    uint32_t    height       = 0;
    bool        has_camera   = false;
    float       eye[3]       = {};   // --camera ex,ey,ez,tx,ty,tz
    float       target[3]    = {};
    float       fov_deg      = 45.0f;  // --fov <deg>
    bool        has_clear    = false;
    float       clear[3]     = {};   // --clear r,g,b
    bool        list_channels = false;

    bool track_enabled() const { return !track_path.empty(); }
    bool raw_to_stdout() const { return raw_out == "-"; }
};

enum class OptionParse { NotMine, Ok, Error };

/// Consume argv[i] (and its value) when it is one of the options above;
/// advances `i` past the value. On Error a message has been printed.
OptionParse parse_track_option(int argc, char** argv, int& i, TrackOptions& out);

/// Cross-option checks that need only TrackOptions (e.g. --raw-out needs
/// --track). Returns false after printing a message.
bool validate_track_options(const TrackOptions& opts);

/// True when argv asks for machine output on stdout (`--raw-out -` or
/// `--list-channels`); checked before anything is logged.
bool wants_clean_stdout(int argc, char** argv, bool& binary);

} // namespace pictor_fbx_viewer
