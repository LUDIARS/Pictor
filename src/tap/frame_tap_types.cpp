#include "pictor/tap/frame_tap_types.h"

namespace pictor {

const char* frame_tap_pass_name(FrameTapPass pass) {
    switch (pass) {
        case FrameTapPass::SCENE: return "scene";
        case FrameTapPass::UI:    return "ui";
    }
    return "scene";
}

} // namespace pictor
