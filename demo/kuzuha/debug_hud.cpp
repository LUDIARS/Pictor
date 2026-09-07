#include "debug_hud.h"
#include <cstdio>
#include <algorithm>
#include <cmath>

namespace pictor_kuzuha {
/// @implements SPEC-PC-KUZUHA-RAYMARCH
void draw_debug_hud(pictor::BitmapTextRenderer& text,const Reconstruction& state,
                    uint32_t unresolved,float frame_ms,unsigned screen_height,bool motion) {
    const auto& o=state.options();
    auto line=[&](float y,const char* value,float r=1.f,float g=1.f,float b=1.f) {
        text.draw_text(17,y+1,value,0,0,0);text.draw_text(16,y,value,r,g,b);
    };
    line(16,o.raymarch?"PICTOR | PN POLYNOMIAL RAYMARCH":"PICTOR | RASTER REFERENCE",.5f,1.f,1.f);
    char info[180];
    std::snprintf(info,sizeof(info),"%.1f FPS | %.1f ms | %zu PN patches",frame_ms>0?1000/frame_ms:0,frame_ms,state.model().patches().size());
    line(38,info);
    std::snprintf(info,sizeof(info),"Interpolation alpha: %.3f [%s]",o.strength,o.animate_strength?"ANIMATED":"MANUAL");
    line(58,info);
    std::snprintf(info,sizeof(info),"LOD: %s | Q%u | target %.3f px",
        o.raymarch?(o.auto_lod?"AUTO":"FIXED"):"RASTER MANUAL",o.factor,state.pixel_tolerance());
    line(78,info);
    if (o.raymarch) {
        float eye[3],center[3];state.camera(eye,center);
        float distance2=0;
        for (int i=0;i<3;++i) distance2+=(eye[i]-center[i])*(eye[i]-center[i]);
        const auto p=state.ray_parameters(1,screen_height);
        const float epsilon=std::max(p.quality[2]*8,
            o.auto_lod?std::sqrt(distance2)*p.quality[0]:p.quality[1]);
        std::snprintf(info,sizeof(info),"Epsilon at target: %.5f model units",epsilon);
        line(116,info,.75f,.85f,1.f);
    }
    std::snprintf(info,sizeof(info),"Unresolved patch-rays: %u",o.raymarch?unresolved:0);
    line(98,info,unresolved?1.f:.6f,unresolved?.3f:1.f,.65f);
    line(148,"F1/F2/F3/F4  LOD quality 1/2/4/8");
    line(166,"A           auto LOD / fixed tolerance");
    line(184,"[ / ]       interpolation - / + (hold)");
    line(202,"I           animate interpolation");
    line(220,"S           source / full interpolation");
    line(238,motion?"Live polynomial motion | raymarch":"F5          raymarch / raster reference");
    line(256,"C           color/clay/normals/steps");
    line(274,"V           head / body");
    line(292,"Arrows      orbit / zoom | Esc: close");
    const float bottom=static_cast<float>(screen_height)-62;
    if (screen_height>=420) {
        line(bottom,"S(u,v,a) = (1-a) * LINEAR(u,v) + a * PN_CUBIC(u,v)",.6f,1.f,1.f);
        line(bottom+18,o.raymarch?"Surface triangle draws: 0 | GPU evaluates the polynomial for every hit":"Reference path: sampled triangles | F5 switches to polynomial raymarch");
        line(bottom+36,"Raymarch: distance lower bounds | magenta = unresolved | FBX-derived coefficients",.85f,.85f,.85f);
    }
}
}
