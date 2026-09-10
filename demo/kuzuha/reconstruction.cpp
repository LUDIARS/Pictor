#include "reconstruction.h"
#include <GLFW/glfw3.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace pictor_kuzuha {
namespace {
/// @implements SPEC-PC-KUZUHA-RAYMARCH
float number(const char* text,float lo,float hi) {
    const std::string parsed=text;size_t end=0;float value=0;
    try { value=std::stof(parsed,&end); }
    catch (const std::exception&) { throw std::runtime_error("Invalid reconstruction numeric option"); }
    if (end!=parsed.size() || !std::isfinite(value) || value<lo || value>hi)
        throw std::runtime_error("Invalid reconstruction numeric option");
    return value;
}
const char* captures[]{"source-head.bmp","pn-head.bmp","source-facets.bmp","pn-facets.bmp",
                       "pn-body.bmp","pn-half.bmp","pn-low-lod.bmp","pn-march-steps.bmp"};
}
/// @implements SPEC-PC-KUZUHA-RAYMARCH
bool parse_option(int argc,char** argv,int& i,Options& o) {
    const std::string arg=argv[i];
    if (arg=="--stage") { o.enabled=true;o.stage=true;return true; }
    if (arg=="--stage-fixed-camera") { o.enabled=true;o.stage=true;o.stage_fixed_camera=true;return true; }
    if (arg=="--stage-orbit-curve") {
        if(++i>=argc) throw std::runtime_error("--stage-orbit-curve needs a file");
        o.stage_orbit_curve=argv[i];o.enabled=true;o.stage=true;return true;
    }
    if (arg=="--animate-interpolation") { o.enabled=true;o.animate_strength=true;return true; }
    if (arg=="--fixed-lod") { o.enabled=true;o.auto_lod=false;return true; }
    if (arg!="--pn-factor" && arg!="--pn-strength" && arg!="--view" && arg!="--yaw" &&
        arg!="--zoom" && arg!="--display" && arg!="--capture-set" && arg!="--pn-report" &&
        arg!="--renderer" && arg!="--window-width" && arg!="--window-height") return false;
    if (++i>=argc) throw std::runtime_error(arg+" needs a value");
    const std::string value=argv[i];o.enabled=true;
    if (arg=="--pn-factor") {
        const float v=number(argv[i],1,8);
        if (v!=1 && v!=2 && v!=4 && v!=8) throw std::runtime_error("PN factor must be 1,2,4,8");
        o.factor=static_cast<unsigned>(v);
    } else if (arg=="--pn-strength") o.strength=number(argv[i],0,1);
    else if (arg=="--yaw") o.yaw=number(argv[i],-360,360);
    else if (arg=="--zoom") o.zoom=number(argv[i],.25f,4);
    else if (arg=="--window-width" || arg=="--window-height") {
        const auto v=number(argv[i],240,3840);
        if (v!=std::floor(v)) throw std::runtime_error("Window size must be an integer");
        (arg=="--window-width"?o.width:o.height)=static_cast<unsigned>(v);
    } else if (arg=="--view") {
        if (value!="head" && value!="body") throw std::runtime_error("View must be head or body");
        o.head=value=="head";
    } else if (arg=="--display") {
        if (value!="texture" && value!="clay" && value!="facets" && value!="steps")
            throw std::runtime_error("Display must be texture,clay,facets,steps");
        o.display=value=="texture"?0u:value=="clay"?1u:value=="facets"?2u:3u;
    } else if (arg=="--renderer") {
        if (value!="raymarch" && value!="raster") throw std::runtime_error("Renderer must be raymarch or raster");
        o.raymarch=value=="raymarch";
    } else if (arg=="--capture-set") o.capture_directory=value;
    else o.report=value;
    return true;
}
/// @implements SPEC-PC-KUZUHA-RAYMARCH
std::string first_capture(Options& o) {
    if (o.capture_directory.empty()) return {};
    std::filesystem::create_directories(o.capture_directory);
    o.factor=4;o.head=true;o.display=0;o.strength=0;o.animate_strength=false;
    return (std::filesystem::path(o.capture_directory)/captures[0]).string();
}
/// @implements SPEC-PC-KUZUHA-RAYMARCH
Reconstruction::Reconstruction(pictor_fbx_viewer::PackedMesh source,Options options)
    :model_(std::move(source)),options_(std::move(options)),stage_orbit_(options_.stage_orbit_curve) {
    min_y_=model_.source().vertices[0].position[1];float max_y=min_y_;
    for (const auto& v:model_.source().vertices) { min_y_=std::min(min_y_,v.position[1]);max_y=std::max(max_y,v.position[1]); }
    height_=max_y-min_y_;
    if (!(height_>0)) throw std::runtime_error("Reconstruction requires non-flat bounds");
    phase_=std::acos(1-2*options_.strength);
}
/// @implements SPEC-PC-KUZUHA-RAYMARCH
pictor_fbx_viewer::PackedMesh Reconstruction::rebuild() {
    auto mesh=model_.sample(options_.raymarch?1:options_.factor,options_.strength,stats_);
    dirty_=false;
    std::printf("[PN] %s alpha=%.3f quality=%u source_patches=%zu raster_triangles=%zu\n",
        options_.raymarch?"POLYNOMIAL RAYMARCH":"RASTER REFERENCE",options_.strength,options_.factor,
        model_.patches().size(),options_.raymarch?size_t{0}:stats_.output_triangles);
    return mesh;
}
/// @implements SPEC-PC-KUZUHA-RAYMARCH
bool Reconstruction::key(int code) {
    if (code>=GLFW_KEY_F1 && code<=GLFW_KEY_F4) {
        options_.factor=1u<<(code-GLFW_KEY_F1);
        dirty_=!options_.raymarch;
    } else if (code==GLFW_KEY_LEFT_BRACKET || code==GLFW_KEY_RIGHT_BRACKET) {
        options_.animate_strength=false;
        options_.strength=std::clamp(options_.strength+(code==GLFW_KEY_LEFT_BRACKET?-.05f:.05f),0.f,1.f);
        dirty_=!options_.raymarch;
    } else if (code==GLFW_KEY_S) {
        options_.animate_strength=false;options_.strength=options_.strength>0?0.f:1.f;dirty_=!options_.raymarch;
    } else if (code==GLFW_KEY_F5) {
        options_.raymarch=!options_.raymarch;options_.animate_strength=false;dirty_=true;
    } else if (code==GLFW_KEY_A) options_.auto_lod=!options_.auto_lod;
    else if (code==GLFW_KEY_I) {
        options_.raymarch=true;options_.animate_strength=!options_.animate_strength;
        phase_=std::acos(1-2*options_.strength);dirty_=true;
    } else if (code==GLFW_KEY_C) options_.display=(options_.display+1)%4;
    else if (code==GLFW_KEY_V) options_.head=!options_.head;
    else if (code==GLFW_KEY_LEFT) options_.yaw-=10;
    else if (code==GLFW_KEY_RIGHT) options_.yaw+=10;
    else if (code==GLFW_KEY_UP) options_.zoom=std::min(options_.zoom*1.15f,4.f);
    else if (code==GLFW_KEY_DOWN) options_.zoom=std::max(options_.zoom/1.15f,.25f);
    else return false;
    return true;
}
/// @implements SPEC-PC-KUZUHA-RAYMARCH
void Reconstruction::update(float dt) {
    if (options_.animate_strength && options_.raymarch) {
        phase_=std::fmod(phase_+std::min(dt,.25f)*.9f,6.2831853f);
        options_.strength=.5f-.5f*std::cos(phase_);
    }
}
/// @implements SPEC-PC-KUZUHA-RAYMARCH
void Reconstruction::camera(float eye[3],float center[3]) const {
    const auto& mesh=model_.source();
    const float targetY=options_.head?min_y_+height_*(options_.stage?.89f:.85f):mesh.center.y;
    const bool moving=options_.stage&&!options_.stage_fixed_camera;
    const float sweep=moving?stage_orbit_.sample(stage_time_):0.f;
    const float push=moving?1.f+.10f*std::sin(stage_time_*.18f):1.f;
    const float distance=(options_.head?height_*.43f:mesh.radius*2.6f)/(options_.zoom*push);
    const float angle=(options_.yaw+12.f*sweep)*3.14159265f/180;
    center[0]=mesh.center.x;center[1]=targetY;center[2]=mesh.center.z;
    eye[0]=center[0]+distance*std::cos(angle);eye[1]=targetY+height_*.015f;eye[2]=center[2]+distance*std::sin(angle);
    const float offset[3]={camera_translation_.x,camera_translation_.y,camera_translation_.z};
    for(unsigned i=0;i<3;++i){
        if(!std::isfinite(offset[i]))throw std::runtime_error("Nonfinite camera translation");
        eye[i]+=offset[i];center[i]+=offset[i];
    }
}
/// @implements SPEC-PC-KUZUHA-RAYMARCH
RaymarchParameters Reconstruction::ray_parameters(unsigned width,unsigned height) const {
    RaymarchParameters p;float eye[3],center[3];camera(eye,center);
    const auto forward=unit({center[0]-eye[0],center[1]-eye[1],center[2]-eye[2]});
    const auto right=unit({-forward.z,0,forward.x});
    const Vec up{right.y*forward.z-right.z*forward.y,right.z*forward.x-right.x*forward.z,
                 right.x*forward.y-right.y*forward.x};
    p.viewport[0]=float(width);p.viewport[1]=float(height);p.viewport[2]=options_.strength;p.viewport[3]=float(options_.display);
    p.right[0]=right.x;p.right[1]=right.y;p.right[2]=right.z;p.right[3]=.414213562f*width/std::max(height,1u);
    p.up[0]=up.x;p.up[1]=up.y;p.up[2]=up.z;p.up[3]=.414213562f;
    p.forward[0]=forward.x;p.forward[1]=forward.y;p.forward[2]=forward.z;
    p.quality[0]=2*.414213562f/std::max(height,1u)*pixel_tolerance();
    p.quality[1]=options_.auto_lod?0.f:height_*.00015f*pixel_tolerance();
    p.quality[2]=std::max(height_*2e-7f,1e-6f);
    return p;
}
/// @implements SPEC-PC-KUZUHA-RAYMARCH
std::string Reconstruction::title() const {
    char text[200];
    std::snprintf(text,sizeof(text),"Pictor | Kuzuha %s | alpha %.2f | quality %u | controls in viewport",
                  options_.raymarch?"PN RAYMARCH":"PN RASTER",options_.strength,options_.factor);
    return text;
}
/// @implements SPEC-PC-KUZUHA-RAYMARCH
void Reconstruction::record_capture(uint32_t unresolved,float frame_ms) const {
    float eye[3],target[3];camera(eye,target);
    std::printf("[Camera capture] time=%.6f eye=%.6f,%.6f,%.6f target=%.6f,%.6f,%.6f\n",
        stage_time_,eye[0],eye[1],eye[2],target[0],target[1],target[2]);
    std::printf("[PN capture] renderer=%s alpha=%.3f tolerance=%.3fpx auto_lod=%d unresolved_previous_frame=%u frame_ms=%.3f\n",
        options_.raymarch?"raymarch":"raster",options_.strength,pixel_tolerance(),options_.auto_lod,unresolved,frame_ms);
    if (options_.report.empty()) return;
    const auto parent=std::filesystem::path(options_.report).parent_path();
    if (!parent.empty()) std::filesystem::create_directories(parent);
    std::ofstream out(options_.report,std::ios::binary|std::ios::app);
    out<<"{\"method\":\"PN polynomial lower-bound sphere tracing\",\"renderer\":\""
       <<(options_.raymarch?"raymarch":"raster")<<"\",\"alpha\":"<<options_.strength
       <<",\"pixelTolerance\":"<<pixel_tolerance()<<",\"autoLod\":"<<(options_.auto_lod?"true":"false")
       <<",\"sourcePatches\":"<<model_.patches().size()<<",\"surfaceTriangles\":"
       <<(options_.raymarch?size_t{0}:stats_.output_triangles)<<",\"unresolvedPreviousFrame\":"<<unresolved
       <<",\"frameMs\":"<<frame_ms<<"}\n";
    out.close();if (!out) throw std::runtime_error("Cannot write PN capture report");
}
/// @implements SPEC-PC-KUZUHA-RAYMARCH
std::string Reconstruction::advance_capture() {
    if (options_.capture_directory.empty() || ++capture_index_>=std::size(captures)) return {};
    options_.factor=capture_index_==6?1u:4u;
    options_.strength=capture_index_==2?0.f:capture_index_==5?.5f:1.f;
    options_.display=capture_index_==2 || capture_index_==3?2u:capture_index_==7?3u:0u;
    options_.head=capture_index_!=4;dirty_=!options_.raymarch;
    return (std::filesystem::path(options_.capture_directory)/captures[capture_index_]).string();
}
}
