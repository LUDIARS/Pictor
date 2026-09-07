#pragma once
#include "reconstruction.h"
#include "pictor/profiler/bitmap_text_renderer.h"
namespace pictor_kuzuha {
void draw_debug_hud(pictor::BitmapTextRenderer& text,const Reconstruction& state,
                    uint32_t unresolved,float frame_ms,unsigned screen_height,bool motion=false);
}
