// =============================================================================
// Pictor — Unite Tokyo 2026『hololive Dreams』背景づくりと 24 時間ライティング 学習デモ
//
//   1 Sky (大気散乱)   2 Night (星・瞬き・天の川)   3 Light Shaft (空気感・遮蔽判定)
//   4 Ocean (Gerstner・浅瀬・水底の色・船の水抜き・コースティクス・LOD)
//   5 Clouds (SixWay・前方散乱・雲専用ライト)   6 GI (SkyOcclusion・GlobalGI・LocalGI)
//   7 Talent (専用ライト・リム・GI 補正)   8 Grass (GPU カリング・風・踏み分け)
//   9 SkyTime / Profile (グラデーション・カーブ・クリップブレンド)
//
// 操作は README.md / HUD を参照 (H で HUD 切替)。
// シェーダーは demo/unite_bg/shaders/、解説はドキュメント側にまとめてある。
// =============================================================================

#include "pictor/pictor_vulkan.h"
#include "pictor/surface/vulkan_context.h"
#include "pictor/surface/glfw_surface_provider.h"
#include "pictor/profiler/bitmap_text_renderer.h"

#include "demo_math.h"
#include "sky_profile.h"
#include "gerstner_cpu.h"
#include "vk_helpers.h"
#include "grass_system.h"

#include <GLFW/glfw3.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace ubg;

namespace {

// ---------------------------------------------------------------------------
// フレーム UBO (shaders/common.glsl の FrameUBO と std140 で一致)
// ---------------------------------------------------------------------------
struct FrameUBO {
    float   view_proj[16];
    float   inv_view_proj[16];
    float   cam_pos[4];
    float   sun_dir[4];
    float   moon_dir[4];
    float   sun_color[4];
    float   moon_color[4];
    float   rayleigh_tint[4];
    float   mie_tint[4];
    float   wavelength[4];
    float   atmos[4];
    float   atmos2[4];
    float   resolution[4];
    float   misc[4];
    float   p[8][4];
    int32_t flags[4];
    float   objs[16][4];
    float   profile_keys[48][4];
    int32_t profile_counts[4];
};
static_assert(sizeof(FrameUBO) % 16 == 0, "std140 size");

// ---------------------------------------------------------------------------
// シーン定義
// ---------------------------------------------------------------------------
struct Param {
    const char* name;
    float*      value;
    float       min_v, max_v, speed;   // speed = 1 秒押し続けた時の変化量
    bool        integer = false;
};

struct Scene {
    const char*  name;
    const char*  frag;              // メインパスのフルスクリーンシェーダー
    float        p[8][4]{};
    float        objs[16][4]{};
    int32_t      flags = 0;
    int32_t      default_flags = 0;
    const char*  toggles[6]{};
    std::vector<Param> params;
    Vec3         cam_pos;
    float        yaw = 0, pitch = 0;
    float        default_time = 0.5f;
    float        exposure = 1.0f;
    VkPipeline   pipeline = VK_NULL_HANDLE;
};

enum SceneId { kSky = 0, kNight, kShaft, kOcean, kClouds, kGI, kTalent, kGrass, kProfile, kSceneCount };

const int kToggleKeys[6] = {GLFW_KEY_Z, GLFW_KEY_X, GLFW_KEY_C, GLFW_KEY_V, GLFW_KEY_N, GLFW_KEY_M};
const char* kToggleKeyNames[6] = {"Z", "X", "C", "V", "N", "M"};

void set4(float* d, float a, float b, float c, float e) { d[0] = a; d[1] = b; d[2] = c; d[3] = e; }

// ---------------------------------------------------------------------------
// アプリ全体の状態
// ---------------------------------------------------------------------------
struct App {
    pictor::GlfwSurfaceProvider surface;
    pictor::VulkanContext       vk;
    pictor::BitmapTextRenderer  text;

    SkyProfile profile = SkyProfile::make_default();
    std::array<Scene, kSceneCount> scenes;
    int   scene = kSky;
    int   selected_param = 0;
    float sky_time = 0.5f;      // SkyTime (0..1)
    bool  time_playing = false;
    float time_speed = 60.0f;   // 分 / 秒
    float clip_a = 12.0f, clip_b = 18.0f, clip_w = 0.5f;   // Scene 9 のクリップブレンド
    bool  hud = true;
    bool  bake_pending = true;
    float exposure = 1.0f;

    Vec3  cam_pos;
    float yaw = 0, pitch = 0;
    double last_mx = 0, last_my = 0;
    bool   dragging = false;
    bool   prev_keys[GLFW_KEY_LAST + 1]{};

    // GPU
    VkRenderPass render_pass = VK_NULL_HANDLE;
    Image depth;
    std::vector<VkFramebuffer> framebuffers;
    std::vector<VkImageView>   fb_views;
    VkExtent2D fb_extent{};

    VkDescriptorSetLayout set0_layout = VK_NULL_HANDLE, set1_layout = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    std::vector<VkDescriptorSet> set0;
    std::vector<Buffer> ubos;
    VkSampler sampler = VK_NULL_HANDLE;

    VkPipelineLayout layout_fs = VK_NULL_HANDLE, layout_grass_gfx = VK_NULL_HANDLE, layout_grass_comp = VK_NULL_HANDLE;
    VkPipeline pipe_grass_ground = VK_NULL_HANDLE, pipe_grass = VK_NULL_HANDLE;
    VkPipeline pipe_grass_env = VK_NULL_HANDLE, pipe_grass_cull = VK_NULL_HANDLE;
    VkPipeline pipe_shaft_scene = VK_NULL_HANDLE, pipe_cloud_bake = VK_NULL_HANDLE;
    VkPipeline pipe_probe_bake = VK_NULL_HANDLE, pipe_local_bake = VK_NULL_HANDLE;

    OffscreenTarget cloud_bake, probe_bake, local_bake, shaft_target;
    GrassSystem grass;

    OceanParams ocean_cpu;
    float boat_state[2][4]{};
};

std::string spv(const char* name) { return std::string("shaders/unite_bg/") + name + ".spv"; }

// ---------------------------------------------------------------------------
// シーンの初期値
// ---------------------------------------------------------------------------
void init_scenes(App& a) {
    auto& atm = a.profile.atmosphere;
    // ---- 1 Sky ----
    {
        Scene& s = a.scenes[kSky];
        s.name = "1 Sky: Atmospheric Scattering"; s.frag = "sky.frag";
        s.toggles[0] = "Rayleigh"; s.toggles[1] = "Mie"; s.toggles[2] = "Tint(Profile)"; s.toggles[3] = "Ground";
        s.default_flags = 0b1111;
        s.params = {
            {"Wavelength R (nm)", &atm.wavelength[0], 380, 780, 60},
            {"Wavelength G (nm)", &atm.wavelength[1], 380, 780, 60},
            {"Wavelength B (nm)", &atm.wavelength[2], 380, 780, 60},
            {"Molecular Density", &atm.molecular_density, 0, 5, 0.5f},
            {"Rayleigh Height (m)", &atm.rayleigh_height, 500, 30000, 2500},
            {"Mie Height (m)", &atm.mie_height, 100, 10000, 600},
            {"Mie Density", &atm.mie_density, 0, 20, 1.5f},
            {"Mie Directionality g", &atm.mie_g, 0, 0.99f, 0.1f},
            {"Scattering Intensity", &atm.scattering_intensity, 0, 80, 6},
        };
        s.cam_pos = {0, 2, 0}; s.yaw = -90; s.pitch = 8; s.default_time = hours(17, 20);
    }
    // ---- 2 Night ----
    {
        Scene& s = a.scenes[kNight];
        s.name = "2 Night Sky: Stars / Twinkle / Milky Way"; s.frag = "night.frag";
        s.toggles[0] = "Twinkle"; s.toggles[1] = "Galaxy"; s.toggles[2] = "Breakdown"; s.toggles[3] = "Profile Curve";
        s.default_flags = 0b1011;
        set4(s.p[0], 120, 1.2f, 0.3f, 0.8f);
        set4(s.p[1], 0.0f, -1.4f, -8.0f, 0.3f);
        set4(s.p[2], 0.0f, 1.0f, 0.18f, 0);
        s.params = {
            {"Star Tiling", &s.p[0][0], 20, 400, 40},
            {"Star Exposure", &s.p[0][1], 0, 10, 1},
            {"Flicker Speed", &s.p[0][2], 0, 3, 0.3f},
            {"Flicker Strength", &s.p[0][3], 0, 1, 0.3f},
            {"Rotation Axis X", &s.p[1][0], -10, 10, 2},
            {"Rotation Axis Y", &s.p[1][1], -10, 10, 2},
            {"Rotation Axis Z", &s.p[1][2], -10, 10, 2},
            {"Rotation Daily Cycle", &s.p[1][3], 0, 3, 0.3f},
            {"Rotation Angle Offset", &s.p[2][0], -180, 180, 45},
            {"Galaxy Exposure", &s.p[2][1], 0, 5, 0.5f},
            {"Galaxy Width", &s.p[2][2], 0.02f, 1, 0.1f},
        };
        s.cam_pos = {0, 2, 0}; s.yaw = 30; s.pitch = 35; s.default_time = hours(23, 30); s.exposure = 4.0f;
    }
    // ---- 3 Light Shaft ----
    {
        Scene& s = a.scenes[kShaft];
        s.name = "3 Air: Haze + Light Shafts"; s.frag = "shaft_composite.frag";
        s.toggles[0] = "Shafts"; s.toggles[1] = "x Atmos Color"; s.toggles[2] = "Occlusion View"; s.toggles[3] = "Jitter";
        s.default_flags = 0b1011;
        set4(s.p[0], 24, 120, 0.6f, 0.97f);
        set4(s.p[1], 0.006f, 0, 0.8f, 0);
        s.params = {
            {"Sample Count", &s.p[0][0], 2, 64, 12, true},
            {"Distance Scale (m)", &s.p[0][1], 5, 1000, 80},
            {"Shaft Intensity", &s.p[0][2], 0, 5, 0.5f},
            {"Decay", &s.p[0][3], 0.8f, 1.0f, 0.03f},
            {"Haze Density", &s.p[1][0], 0, 0.05f, 0.005f},
            {"Screen March Length", &s.p[1][2], 0.05f, 1, 0.2f},
        };
        for (int i = 0; i < 10; ++i) {
            float x = -25.0f - float(i % 5) * 14.0f;
            float z = -10.0f + float(i / 5) * 22.0f + 6.0f * std::sin(float(i) * 1.3f);
            set4(s.objs[i], x, 0, z, 10.0f + 14.0f * std::fabs(std::sin(float(i) * 2.7f)));
        }
        s.cam_pos = {25, 1.7f, 8}; s.yaw = -95; s.pitch = 6; s.default_time = hours(17, 25);
    }
    // ---- 4 Ocean ----
    {
        Scene& s = a.scenes[kOcean];
        s.name = "4 Ocean: Gerstner / Shallow / Caustics"; s.frag = "ocean.frag";
        s.toggles[0] = "Shallow Damp"; s.toggles[1] = "Deep Floor"; s.toggles[2] = "Boat Mask";
        s.toggles[3] = "LOD Tiles"; s.toggles[4] = "Caustic SunColor"; s.toggles[5] = "Caustic Normal";
        s.default_flags = 0b110111;
        set4(s.p[0], 1.0f, 0.6f, 1.0f, 30.0f);
        set4(s.p[1], 3.0f, 1.0f, 0.6f, 1.2f);
        set4(s.p[2], 1.0f, 150.0f, 400.0f, 0.9f);
        s.params = {
            {"Wave Amplitude", &s.p[0][0], 0, 3, 0.4f},
            {"Wave Steepness Q", &s.p[0][1], 0, 1, 0.2f},
            {"Wavelength Scale", &s.p[0][2], 0.3f, 3, 0.3f},
            {"Wind Direction (deg)", &s.p[0][3], -180, 180, 45},
            {"Shallow Depth (m)", &s.p[1][0], 0.1f, 10, 1},
            {"Absorption", &s.p[1][1], 0, 4, 0.4f},
            {"Deep Floor", &s.p[1][2], 0, 2, 0.3f},
            {"Caustics Strength", &s.p[1][3], 0, 4, 0.4f},
            {"Foam", &s.p[2][0], 0, 3, 0.4f},
            {"Wave Fade Start (m)", &s.p[2][1], 10, 600, 40},
            {"Wave Fade End (m)", &s.p[2][2], 20, 1200, 60},
            {"Caustics Scale", &s.p[2][3], 0.1f, 3, 0.2f},
        };
        s.cam_pos = {-2, 3.2f, -12}; s.yaw = 10; s.pitch = -9; s.default_time = hours(10, 30);
    }
    // ---- 5 Clouds ----
    {
        Scene& s = a.scenes[kClouds];
        s.name = "5 Clouds: SixWay / Forward Scatter / Cloud Light"; s.frag = "clouds.frag";
        s.toggles[0] = "SixWay"; s.toggles[1] = "Forward Scatter"; s.toggles[2] = "Cloud Light"; s.toggles[3] = "Show Bake";
        s.default_flags = 0b0111;
        set4(s.p[0], 3.0f, 1.5f, 8.0f, 0.6f);
        set4(s.p[1], 0.05f, 0.75f, 0, 0);
        s.params = {
            {"Cloud Light Intensity", &s.p[0][0], 0, 10, 1},
            {"Forward Scatter", &s.p[0][1], 0, 6, 0.6f},
            {"Forward Power", &s.p[0][2], 1, 64, 6},
            {"Ambient", &s.p[0][3], 0, 3, 0.3f},
            {"Cloud Light Hue", &s.p[1][0], 0, 1, 0.08f},
            {"Cloud Light Saturation", &s.p[1][1], 0, 1, 0.15f},
        };
        for (int i = 0; i < 12; ++i) {
            float ang = -kPi * 0.5f + (float(i) - 5.5f) * 0.12f;      // 西側に並べる
            float dist = 380.0f + 420.0f * std::fabs(std::sin(float(i) * 1.9f));
            set4(s.objs[i], std::sin(ang) * dist, 90.0f + 140.0f * std::fabs(std::sin(float(i) * 0.7f)),
                 std::cos(ang) * dist, 90.0f + 80.0f * std::fabs(std::cos(float(i) * 1.3f)));
        }
        s.objs[12][0] = 12;
        s.cam_pos = {0, 20, 0}; s.yaw = -90; s.pitch = 9; s.default_time = hours(17, 50);
    }
    // ---- 6 GI ----
    {
        Scene& s = a.scenes[kGI];
        s.name = "6 GI: SkyOcclusion x GlobalGI / LocalGI"; s.frag = "gi.frag";
        s.toggles[0] = "SkyOcclusion"; s.toggles[1] = "Ground Override"; s.toggles[2] = "LocalGI";
        s.toggles[3] = "Show Probes"; s.toggles[4] = "Moving Box"; s.toggles[5] = "GI Only";
        s.default_flags = 0b000111;
        set4(s.p[0], 1.0f, 2.0f, 1.0f, 0.18f);
        s.params = {
            {"GI Intensity", &s.p[0][0], 0, 4, 0.4f},
            {"Bulb Intensity", &s.p[0][1], 0, 10, 1},
            {"Local Blend Dist (m)", &s.p[0][2], 0.01f, 3, 0.4f},
            {"Probe View Size", &s.p[0][3], 0.05f, 0.6f, 0.1f},
        };
        s.cam_pos = {0, 4.5f, -15}; s.yaw = 0; s.pitch = -12; s.default_time = hours(15, 0);
    }
    // ---- 7 Talent ----
    {
        Scene& s = a.scenes[kTalent];
        s.name = "7 Talent: Light / Rim / GI Correction"; s.frag = "talent.frag";
        s.toggles[0] = "Rim"; s.toggles[1] = "GI Correction"; s.toggles[2] = "Roof (shadow)"; s.toggles[3] = "Turntable";
        s.default_flags = 0b0011;
        set4(s.p[0], 0, 60, 30, 3.0f);
        set4(s.p[1], 1.5f, 0.2f, 1.0f, 1.0f);
        set4(s.p[2], 0.7f, 1.0f, 0, 0);
        s.params = {
            {"Light Mode 0/1/2", &s.p[0][0], 0, 2, 1.5f, true},
            {"Offset Yaw (deg)", &s.p[0][1], -180, 180, 45},
            {"Fixed Yaw (deg)", &s.p[0][2], -180, 180, 45},
            {"Rim Power", &s.p[0][3], 0.5f, 10, 1},
            {"Rim Intensity", &s.p[1][0], 0, 5, 0.5f},
            {"Rim In Shadow", &s.p[1][1], 0, 1, 0.2f},
            {"GI Intensity", &s.p[1][2], 0, 3, 0.3f},
            {"GI Saturation", &s.p[1][3], 0, 2, 0.3f},
            {"GI Correction", &s.p[2][0], 0, 1, 0.2f},
            {"GI Boost", &s.p[2][1], 0, 3, 0.3f},
        };
        s.cam_pos = {0, 1.15f, -3.4f}; s.yaw = 0; s.pitch = -3; s.default_time = hours(16, 30);
    }
    // ---- 8 Grass ----
    {
        Scene& s = a.scenes[kGrass];
        s.name = "8 Grass: GPU Culling / Wind / Trample"; s.frag = "grass_ground.frag";
        s.toggles[0] = "GPU Cull"; s.toggles[1] = "Cell Colors"; s.toggles[2] = "Round Normal";
        s.toggles[3] = "Trample"; s.toggles[4] = "Recalc From Original";
        s.default_flags = 0b11101;
        set4(s.p[0], 50, 150, 30, 0.1f);
        set4(s.p[1], 0.3f, 0.12f, 20, 0.1f);
        set4(s.p[2], 1.0f, 0.5f, 0.6f, 0.5f);
        set4(s.p[3], 0.5f, 0, 0, 0);
        s.params = {
            {"Fade Start (m)", &s.p[0][0], 5, 250, 15},
            {"Fade End (m)", &s.p[0][1], 10, 300, 15},
            {"LOD0 Distance (m)", &s.p[0][2], 2, 150, 8},
            {"Twist Amount", &s.p[0][3], 0, 2, 0.3f},
            {"Weak Wind TimeScale", &s.p[1][0], 0, 3, 0.3f},
            {"Weak Sway", &s.p[1][1], 0, 1, 0.08f},
            {"Wind Angle (deg)", &s.p[1][2], -180, 180, 45},
            {"Direction Lean", &s.p[1][3], 0, 1, 0.08f},
            {"Direction TimeScale", &s.p[3][0], 0, 3, 0.3f},
            {"Trample Radius", &s.p[2][0], 0.2f, 5, 0.6f},
            {"Trample Strength", &s.p[2][1], 0, 1.5f, 0.3f},
            {"Roundness", &s.p[2][2], 0, 2, 0.3f},
            {"AO", &s.p[2][3], 0, 1, 0.15f},
        };
        s.cam_pos = {0, 2.0f, -22}; s.yaw = 5; s.pitch = -6; s.default_time = hours(16, 0);
    }
    // ---- 9 Profile ----
    {
        Scene& s = a.scenes[kProfile];
        s.name = "9 SkyTime / Profile / Timeline Blend"; s.frag = "profile.frag";
        s.toggles[0] = "Use Clip Blend"; s.toggles[1] = "Sunlight Fixed"; s.toggles[2] = "Star Fixed"; s.toggles[3] = "Rayleigh Fixed";
        s.default_flags = 0;
        s.params = {
            {"Clip A time (h)", &a.clip_a, 0, 24, 3},
            {"Clip B time (h)", &a.clip_b, 0, 24, 3},
            {"Clip B weight", &a.clip_w, 0, 1, 0.25f},
            {"Time Speed (min/s)", &a.time_speed, 0, 1440, 120},
        };
        s.cam_pos = {0, 2, 0}; s.default_time = hours(15, 0);
    }
    for (auto& s : a.scenes) s.flags = s.default_flags;
}

void enter_scene(App& a, int id, bool reset_params) {
    a.scene = id;
    Scene& s = a.scenes[id];
    a.cam_pos = s.cam_pos; a.yaw = s.yaw; a.pitch = s.pitch;
    a.sky_time = s.default_time;
    a.exposure = s.exposure;
    a.selected_param = 0;
    if (reset_params) s.flags = s.default_flags;
}

// ---------------------------------------------------------------------------
// GPU リソース
// ---------------------------------------------------------------------------
bool create_main_targets(App& a) {
    VkDevice dev = a.vk.device();
    VkExtent2D ext = a.vk.swapchain_extent();
    a.depth.destroy(dev);
    for (auto fb : a.framebuffers) vkDestroyFramebuffer(dev, fb, nullptr);
    a.framebuffers.clear();
    if (!a.depth.create(a.vk, ext.width, ext.height, VK_FORMAT_D32_SFLOAT,
                        VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, VK_IMAGE_ASPECT_DEPTH_BIT)) return false;
    a.fb_views = a.vk.swapchain_image_views();
    for (VkImageView v : a.fb_views) {
        VkImageView atts[2] = {v, a.depth.view};
        VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fi.renderPass = a.render_pass;
        fi.attachmentCount = 2;
        fi.pAttachments = atts;
        fi.width = ext.width; fi.height = ext.height; fi.layers = 1;
        VkFramebuffer fb;
        if (vkCreateFramebuffer(dev, &fi, nullptr, &fb) != VK_SUCCESS) return false;
        a.framebuffers.push_back(fb);
    }
    a.fb_extent = ext;

    // ライトシャフト用のオフスクリーン (画面サイズ)
    a.shaft_target.destroy(dev);
    if (!a.shaft_target.create(a.vk, ext.width, ext.height, 1, VK_FORMAT_R16G16B16A16_SFLOAT)) return false;
    a.shaft_target.transition_to_read(a.vk);
    for (VkDescriptorSet s : a.set0) {
        VkDescriptorImageInfo ii{a.sampler, a.shaft_target.colors[0].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = s; w.dstBinding = 7; w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w.pImageInfo = &ii;
        vkUpdateDescriptorSets(dev, 1, &w, 0, nullptr);
    }
    return true;
}

bool create_render_pass(App& a) {
    VkAttachmentDescription atts[2]{};
    atts[0].format = a.vk.swapchain_format();
    atts[0].samples = VK_SAMPLE_COUNT_1_BIT;
    atts[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    atts[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    atts[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    atts[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    atts[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    atts[0].finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    atts[1].format = VK_FORMAT_D32_SFLOAT;
    atts[1].samples = VK_SAMPLE_COUNT_1_BIT;
    atts[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    atts[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    atts[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    atts[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    atts[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    atts[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    VkAttachmentReference cref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference dref{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &cref;
    sub.pDepthStencilAttachment = &dref;
    VkSubpassDependency dep{};
    dep.srcSubpass = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass = 0;
    dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dep.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    VkRenderPassCreateInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rp.attachmentCount = 2;
    rp.pAttachments = atts;
    rp.subpassCount = 1;
    rp.pSubpasses = &sub;
    rp.dependencyCount = 1;
    rp.pDependencies = &dep;
    return vkCreateRenderPass(a.vk.device(), &rp, nullptr, &a.render_pass) == VK_SUCCESS;
}

bool create_descriptors(App& a) {
    VkDevice dev = a.vk.device();
    const uint32_t flights = a.vk.frames_in_flight();

    // set 0: UBO + 7 枚のテクスチャ
    VkDescriptorSetLayoutBinding b0[8]{};
    b0[0] = {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_ALL, nullptr};
    for (uint32_t i = 1; i < 8; ++i)
        b0[i] = {i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount = 8; li.pBindings = b0;
    if (vkCreateDescriptorSetLayout(dev, &li, nullptr, &a.set0_layout) != VK_SUCCESS) return false;

    // set 1: 草のストレージバッファ 4 本
    VkDescriptorSetLayoutBinding b1[4]{};
    for (uint32_t i = 0; i < 4; ++i)
        b1[i] = {i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                 VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    li.bindingCount = 4; li.pBindings = b1;
    if (vkCreateDescriptorSetLayout(dev, &li, nullptr, &a.set1_layout) != VK_SUCCESS) return false;

    VkDescriptorPoolSize sizes[3] = {
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, flights},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, flights * 7},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, flights * 4}};
    VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pi.maxSets = flights * 2;
    pi.poolSizeCount = 3;
    pi.pPoolSizes = sizes;
    if (vkCreateDescriptorPool(dev, &pi, nullptr, &a.pool) != VK_SUCCESS) return false;

    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = VK_FILTER_LINEAR; si.minFilter = VK_FILTER_LINEAR;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.maxLod = 0.0f;
    if (vkCreateSampler(dev, &si, nullptr, &a.sampler) != VK_SUCCESS) return false;

    // ベイク先 (サイズ固定)
    if (!a.cloud_bake.create(a.vk, 512, 256, 2, VK_FORMAT_R16G16B16A16_SFLOAT)) return false;
    if (!a.probe_bake.create(a.vk, 17, 4 * 13, 1, VK_FORMAT_R16G16B16A16_SFLOAT)) return false;
    if (!a.local_bake.create(a.vk, 6, 1, 3, VK_FORMAT_R16G16B16A16_SFLOAT)) return false;
    a.cloud_bake.transition_to_read(a.vk);
    a.probe_bake.transition_to_read(a.vk);
    a.local_bake.transition_to_read(a.vk);

    a.ubos.resize(flights);
    a.set0.resize(flights);
    for (uint32_t f = 0; f < flights; ++f) {
        if (!a.ubos[f].create(a.vk, sizeof(FrameUBO), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, true)) return false;
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = a.pool;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &a.set0_layout;
        if (vkAllocateDescriptorSets(dev, &ai, &a.set0[f]) != VK_SUCCESS) return false;

        VkDescriptorBufferInfo bi{a.ubos[f].buffer, 0, sizeof(FrameUBO)};
        VkImageView views[6] = {a.cloud_bake.colors[0].view, a.cloud_bake.colors[1].view, a.probe_bake.colors[0].view,
                                a.local_bake.colors[0].view, a.local_bake.colors[1].view, a.local_bake.colors[2].view};
        VkDescriptorImageInfo ii[6];
        VkWriteDescriptorSet w[7]{};
        w[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[0].dstSet = a.set0[f]; w[0].dstBinding = 0; w[0].descriptorCount = 1;
        w[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        w[0].pBufferInfo = &bi;
        for (int i = 0; i < 6; ++i) {
            ii[i] = {a.sampler, views[i], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            w[i + 1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[i + 1].dstSet = a.set0[f];
            w[i + 1].dstBinding = static_cast<uint32_t>(i + 1);
            w[i + 1].descriptorCount = 1;
            w[i + 1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            w[i + 1].pImageInfo = &ii[i];
        }
        vkUpdateDescriptorSets(dev, 7, w, 0, nullptr);
    }
    return true;
}

bool create_pipelines(App& a) {
    VkDevice dev = a.vk.device();
    VkPipelineLayoutCreateInfo li{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    li.setLayoutCount = 1;
    li.pSetLayouts = &a.set0_layout;
    if (vkCreatePipelineLayout(dev, &li, nullptr, &a.layout_fs) != VK_SUCCESS) return false;

    VkDescriptorSetLayout both[2] = {a.set0_layout, a.set1_layout};
    VkPushConstantRange gfx_pc{VK_SHADER_STAGE_VERTEX_BIT, 0, 8};
    li.setLayoutCount = 2; li.pSetLayouts = both;
    li.pushConstantRangeCount = 1; li.pPushConstantRanges = &gfx_pc;
    if (vkCreatePipelineLayout(dev, &li, nullptr, &a.layout_grass_gfx) != VK_SUCCESS) return false;
    VkPushConstantRange comp_pc{VK_SHADER_STAGE_COMPUTE_BIT, 0, 24};
    li.pPushConstantRanges = &comp_pc;
    if (vkCreatePipelineLayout(dev, &li, nullptr, &a.layout_grass_comp) != VK_SUCCESS) return false;

    const std::string vs = spv("fullscreen.vert");
    GfxState fs_state;     // メインパス: 深度なし
    for (auto& s : a.scenes) {
        if (&s == &a.scenes[kGrass]) continue;   // 草シーンは下で深度つきの専用パイプラインを作る
        s.pipeline = create_graphics_pipeline(dev, a.render_pass, a.layout_fs, vs, spv(s.frag), fs_state);
        if (!s.pipeline) { fprintf(stderr, "[unite_bg] pipeline failed: %s\n", s.frag); return false; }
    }
    // 草シーン: 背景はレイマーチ結果の深度を書く → 草は深度テスト
    GfxState ground; ground.depth_test = true; ground.depth_write = true; ground.depth_op = VK_COMPARE_OP_ALWAYS;
    a.pipe_grass_ground = create_graphics_pipeline(dev, a.render_pass, a.layout_fs, vs, spv("grass_ground.frag"), ground);
    GfxState grass; grass.depth_test = true; grass.depth_write = true; grass.depth_op = VK_COMPARE_OP_LESS_OR_EQUAL;
    a.pipe_grass = create_graphics_pipeline(dev, a.render_pass, a.layout_grass_gfx, spv("grass.vert"), spv("grass.frag"), grass);
    a.pipe_grass_env = create_compute_pipeline(dev, a.layout_grass_comp, spv("grass_env.comp"));
    a.pipe_grass_cull = create_compute_pipeline(dev, a.layout_grass_comp, spv("grass_cull.comp"));

    // オフスクリーン (深度なし)
    GfxState off; off.has_depth_attachment = false;
    a.pipe_shaft_scene = create_graphics_pipeline(dev, a.shaft_target.render_pass, a.layout_fs, vs, spv("shaft_scene.frag"), off);
    off.color_count = 2;
    a.pipe_cloud_bake = create_graphics_pipeline(dev, a.cloud_bake.render_pass, a.layout_fs, vs, spv("cloud_bake.frag"), off);
    off.color_count = 1;
    a.pipe_probe_bake = create_graphics_pipeline(dev, a.probe_bake.render_pass, a.layout_fs, vs, spv("gi_probe_bake.frag"), off);
    off.color_count = 3;
    a.pipe_local_bake = create_graphics_pipeline(dev, a.local_bake.render_pass, a.layout_fs, vs, spv("gi_local_bake.frag"), off);

    return a.pipe_grass_ground && a.pipe_grass && a.pipe_grass_env && a.pipe_grass_cull &&
           a.pipe_shaft_scene && a.pipe_cloud_bake && a.pipe_probe_bake && a.pipe_local_bake;
}

// ---------------------------------------------------------------------------
// 入力
// ---------------------------------------------------------------------------
bool pressed(App& a, GLFWwindow* w, int key) {
    bool down = glfwGetKey(w, key) == GLFW_PRESS;
    bool edge = down && !a.prev_keys[key];
    a.prev_keys[key] = down;
    return edge;
}

void handle_input(App& a, float dt) {
    GLFWwindow* w = a.surface.glfw_window();
    if (glfwGetKey(w, GLFW_KEY_ESCAPE) == GLFW_PRESS) glfwSetWindowShouldClose(w, 1);
    bool shift = glfwGetKey(w, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS || glfwGetKey(w, GLFW_KEY_RIGHT_SHIFT) == GLFW_PRESS;

    for (int i = 0; i < kSceneCount; ++i)
        if (pressed(a, w, GLFW_KEY_1 + i)) enter_scene(a, i, false);
    Scene& s = a.scenes[a.scene];

    if (pressed(a, w, GLFW_KEY_R)) {   // シーンを初期状態へ (パラメータは再起動で戻る)
        enter_scene(a, a.scene, true);
    }
    if (pressed(a, w, GLFW_KEY_H)) a.hud = !a.hud;
    if (pressed(a, w, GLFW_KEY_T)) a.time_playing = !a.time_playing;
    if (pressed(a, w, GLFW_KEY_B)) a.bake_pending = true;
    for (int i = 0; i < 6; ++i)
        if (pressed(a, w, kToggleKeys[i]) && s.toggles[i]) s.flags ^= (1 << i);
    if (a.scene == kGrass && pressed(a, w, GLFW_KEY_G)) {
        // 建物: なし → 建築中 (大きい Collider) → 完成 (小さい Collider) → なし
        int next = (static_cast<int>(a.grass.building()) + 1) % 3;
        bool from_original = (s.flags & (1 << 4)) != 0;
        a.vk.device_wait_idle();
        a.grass.set_building(static_cast<BuildingState>(next), from_original);
        a.grass.upload();
    }

    // パラメータ
    if (!s.params.empty()) {
        if (pressed(a, w, GLFW_KEY_DOWN)) a.selected_param = (a.selected_param + 1) % int(s.params.size());
        if (pressed(a, w, GLFW_KEY_UP))   a.selected_param = (a.selected_param + int(s.params.size()) - 1) % int(s.params.size());
        Param& p = s.params[std::min<size_t>(a.selected_param, s.params.size() - 1)];
        float dir = 0.0f;
        if (glfwGetKey(w, GLFW_KEY_RIGHT) == GLFW_PRESS) dir += 1.0f;
        if (glfwGetKey(w, GLFW_KEY_LEFT) == GLFW_PRESS)  dir -= 1.0f;
        if (p.integer) {
            if (pressed(a, w, GLFW_KEY_PAGE_UP))   *p.value = clampf(std::round(*p.value) + 1, p.min_v, p.max_v);
            if (pressed(a, w, GLFW_KEY_PAGE_DOWN)) *p.value = clampf(std::round(*p.value) - 1, p.min_v, p.max_v);
        }
        if (dir != 0.0f) *p.value = clampf(*p.value + dir * p.speed * dt * (shift ? 4.0f : 1.0f), p.min_v, p.max_v);
    }

    // 時刻
    float scrub = 0.0f;
    if (glfwGetKey(w, GLFW_KEY_RIGHT_BRACKET) == GLFW_PRESS) scrub += 1.0f;
    if (glfwGetKey(w, GLFW_KEY_LEFT_BRACKET) == GLFW_PRESS)  scrub -= 1.0f;
    a.sky_time += scrub * dt * (shift ? 4.0f : 1.0f) / 24.0f;      // 1 秒 = 1 時間
    if (a.time_playing) a.sky_time += dt * a.time_speed / 1440.0f;
    a.sky_time -= std::floor(a.sky_time);

    // 露出
    if (glfwGetKey(w, GLFW_KEY_EQUAL) == GLFW_PRESS) a.exposure *= 1.0f + dt;
    if (glfwGetKey(w, GLFW_KEY_MINUS) == GLFW_PRESS) a.exposure /= 1.0f + dt;

    // カメラ: 右ドラッグで向き、WASD / Q E で移動
    double mx, my;
    glfwGetCursorPos(w, &mx, &my);
    if (glfwGetMouseButton(w, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS) {
        if (a.dragging) {
            a.yaw += float(mx - a.last_mx) * 0.15f;
            a.pitch = clampf(a.pitch - float(my - a.last_my) * 0.15f, -89.0f, 89.0f);
        }
        a.dragging = true;
    } else {
        a.dragging = false;
    }
    a.last_mx = mx; a.last_my = my;

    float yr = a.yaw * kPi / 180.0f, pr = a.pitch * kPi / 180.0f;
    Vec3 fwd{std::cos(pr) * std::sin(yr), std::sin(pr), std::cos(pr) * std::cos(yr)};
    Vec3 right = normalize(cross(fwd, Vec3{0, 1, 0}));
    float speed = (shift ? 40.0f : 8.0f) * dt;
    if (glfwGetKey(w, GLFW_KEY_W) == GLFW_PRESS) a.cam_pos = a.cam_pos + fwd * speed;
    if (glfwGetKey(w, GLFW_KEY_S) == GLFW_PRESS) a.cam_pos = a.cam_pos - fwd * speed;
    if (glfwGetKey(w, GLFW_KEY_D) == GLFW_PRESS) a.cam_pos = a.cam_pos + right * speed;
    if (glfwGetKey(w, GLFW_KEY_A) == GLFW_PRESS) a.cam_pos = a.cam_pos - right * speed;
    if (glfwGetKey(w, GLFW_KEY_E) == GLFW_PRESS) a.cam_pos.y += speed;
    if (glfwGetKey(w, GLFW_KEY_Q) == GLFW_PRESS) a.cam_pos.y -= speed;
}

// ---------------------------------------------------------------------------
// 毎フレームの UBO
// ---------------------------------------------------------------------------
void fill_profile_keys(const SkyProfile& pr, FrameUBO& u) {
    const GradientTrack* grads[3] = {&pr.sunlight_color, &pr.rayleigh_color, &pr.mie_color};
    for (int t = 0; t < 3; ++t) {
        int n = static_cast<int>(std::min<size_t>(grads[t]->keys.size(), 12));
        if (grads[t]->use_fixed) {
            set4(u.profile_keys[t * 12], grads[t]->fixed_value.x, grads[t]->fixed_value.y, grads[t]->fixed_value.z, 0.0f);
            n = 1;
        } else {
            for (int i = 0; i < n; ++i) {
                const auto& k = grads[t]->keys[i];
                set4(u.profile_keys[t * 12 + i], k.color.x, k.color.y, k.color.z, k.time01);
            }
        }
        u.profile_counts[t] = n;
    }
    const CurveTrack& star = pr.star_intensity;
    int n = static_cast<int>(std::min<size_t>(star.keys.size(), 12));
    if (star.use_fixed) { set4(u.profile_keys[36], star.fixed_value, 0, 0, 0); n = 1; }
    else for (int i = 0; i < n; ++i) set4(u.profile_keys[36 + i], star.keys[i].value, 0, 0, star.keys[i].time01);
    u.profile_counts[3] = n;
}

void update_scene_objects(App& a, float time, float dt) {
    Scene& s = a.scenes[a.scene];
    if (a.scene == kOcean) {
        // 浮力: GPU と同じ式で CPU から水面の高さを取り、船の高さと傾きを決める
        a.ocean_cpu.amplitude = s.p[0][0]; a.ocean_cpu.steepness = s.p[0][1];
        a.ocean_cpu.wave_scale = s.p[0][2]; a.ocean_cpu.wind_deg = s.p[0][3];
        a.ocean_cpu.fade_start = s.p[2][1]; a.ocean_cpu.fade_end = s.p[2][2];
        const float bx = 3.0f, bz = 4.0f, yaw = 0.4f;
        float cx = a.cam_pos.x, cz = a.cam_pos.z;
        float fx = std::sin(yaw) * 1.8f, fz = std::cos(yaw) * 1.8f;    // 船首方向
        float sx = std::cos(yaw) * 0.8f, sz = -std::sin(yaw) * 0.8f;   // 右舷方向
        float hc = water_height_cpu(a.ocean_cpu, bx, bz, time, cx, cz);
        float hf = water_height_cpu(a.ocean_cpu, bx + fx, bz + fz, time, cx, cz);
        float hb = water_height_cpu(a.ocean_cpu, bx - fx, bz - fz, time, cx, cz);
        float hr = water_height_cpu(a.ocean_cpu, bx + sx, bz + sz, time, cx, cz);
        float hl = water_height_cpu(a.ocean_cpu, bx - sx, bz - sz, time, cx, cz);
        set4(s.objs[0], bx, (hc + hf + hb) / 3.0f - 0.05f, bz, yaw);
        set4(s.objs[1], -std::atan2(hf - hb, 3.6f), std::atan2(hr - hl, 1.6f), 0, 0);
    }
    if (a.scene == kClouds) {
        Vec3 col = hsv_to_rgb(s.p[1][0], s.p[1][1], 1.0f);
        set4(s.p[2], col.x, col.y, col.z, 0);
        // 遠い順に並べる (奥から合成するため)
        int n = int(s.objs[12][0]);
        std::vector<std::array<float, 4>> c(n);
        for (int i = 0; i < n; ++i) std::memcpy(c[i].data(), s.objs[i], sizeof(float) * 4);
        std::sort(c.begin(), c.end(), [&](const auto& x, const auto& y) {
            Vec3 px{x[0], x[1], x[2]}, py{y[0], y[1], y[2]};
            return length(px - a.cam_pos) > length(py - a.cam_pos);
        });
        for (int i = 0; i < n; ++i) std::memcpy(s.objs[i], c[i].data(), sizeof(float) * 4);
    }
    if (a.scene == kGI) {
        set4(s.objs[0], 1.5f + 3.0f * std::sin(time * 0.4f), 0.8f, -1.0f + 2.0f * std::cos(time * 0.3f), 0.7f);
    }
    if (a.scene == kTalent) {
        if (s.flags & (1 << 3)) s.objs[0][0] += 0.5f * dt;
    }
    if (a.scene == kGrass) {
        Vec3 c = a.grass.building_center(), h = a.grass.building_half();
        set4(s.objs[0], c.x, 0, c.z, float(static_cast<int>(a.grass.building())));
        set4(s.objs[1], h.x, h.y, h.z, 0);
        set4(s.objs[2], 3.0f * std::sin(time * 0.5f), 0, -8.0f + 3.0f * std::cos(time * 0.5f), 0);
    }
}

void fill_ubo(App& a, FrameUBO& u, float time, float dt) {
    std::memset(&u, 0, sizeof(u));
    Scene& s = a.scenes[a.scene];
    VkExtent2D ext = a.vk.swapchain_extent();
    float aspect = ext.height ? float(ext.width) / float(ext.height) : 1.0f;

    float yr = a.yaw * kPi / 180.0f, pr = a.pitch * kPi / 180.0f;
    Vec3 fwd{std::cos(pr) * std::sin(yr), std::sin(pr), std::cos(pr) * std::cos(yr)};
    Mat4 view = look_at(a.cam_pos, a.cam_pos + fwd, {0, 1, 0});
    Mat4 proj = perspective_vk(60.0f * kPi / 180.0f, aspect, 0.1f, 3000.0f);
    Mat4 vp = mul(proj, view), inv{};
    invert(vp, inv);
    std::memcpy(u.view_proj, vp.m, sizeof(vp.m));
    std::memcpy(u.inv_view_proj, inv.m, sizeof(inv.m));

    // Scene 9 のトグルで Profile の「固定値」を切り替える
    Scene& sp = a.scenes[kProfile];
    a.profile.sunlight_color.use_fixed = (sp.flags & (1 << 1)) != 0;
    a.profile.sunlight_color.fixed_value = {1.0f, 0.95f, 0.85f};
    a.profile.star_intensity.use_fixed = (sp.flags & (1 << 2)) != 0;
    a.profile.star_intensity.fixed_value = 1.0f;
    a.profile.rayleigh_color.use_fixed = (sp.flags & (1 << 3)) != 0;
    a.profile.rayleigh_color.fixed_value = {1.0f, 1.0f, 1.0f};

    // SkyTime: Scene 9 でクリップブレンドを使う場合は「時刻 x weight の和」
    float t = a.sky_time;
    if (sp.flags & 1) t = blend_clip_times(a.clip_a / 24.0f, 1.0f - a.clip_w, a.clip_b / 24.0f, a.clip_w);
    t = t - std::floor(t);

    const SkyProfile& P = a.profile;
    Vec3 sun = sun_direction(t), moon = moon_direction(t);
    Vec3 sc = P.sunlight_color.evaluate(t), mc = P.moonlight_color.evaluate(t);
    Vec3 rt = P.rayleigh_color.evaluate(t), mt = P.mie_color.evaluate(t);
    float moon_i = P.moonlight_intensity.evaluate(t) * (moon.y > -0.05f ? 1.0f : 0.0f);

    set4(u.cam_pos, a.cam_pos.x, a.cam_pos.y, a.cam_pos.z, time);
    set4(u.sun_dir, sun.x, sun.y, sun.z, t);
    set4(u.moon_dir, moon.x, moon.y, moon.z, moon_i);
    set4(u.sun_color, sc.x, sc.y, sc.z, P.sunlight_intensity.evaluate(t));
    set4(u.moon_color, mc.x, mc.y, mc.z, 0);
    set4(u.rayleigh_tint, rt.x, rt.y, rt.z, 0);
    set4(u.mie_tint, mt.x, mt.y, mt.z, 0);
    const AtmosphereSettings& at = P.atmosphere;
    set4(u.wavelength, at.wavelength[0], at.wavelength[1], at.wavelength[2], at.molecular_density);
    set4(u.atmos, at.rayleigh_height, at.mie_height, at.mie_g, at.scattering_intensity);
    set4(u.atmos2, at.mie_density, P.star_intensity.evaluate(t), P.galaxy_intensity.evaluate(t), a.exposure);
    set4(u.resolution, float(ext.width), float(ext.height), 1.0f / float(ext.width), 1.0f / float(ext.height));
    bool srgb = a.vk.swapchain_format() == VK_FORMAT_B8G8R8A8_SRGB || a.vk.swapchain_format() == VK_FORMAT_R8G8B8A8_SRGB;
    set4(u.misc, srgb ? 0.0f : 1.0f, dt, float(a.scene), 0);
    std::memcpy(u.p, s.p, sizeof(u.p));
    u.flags[0] = s.flags;
    std::memcpy(u.objs, s.objs, sizeof(u.objs));
    fill_profile_keys(P, u);
}

// ---------------------------------------------------------------------------
// HUD
// ---------------------------------------------------------------------------
void draw_hud(App& a, VkCommandBuffer cmd) {
    VkExtent2D ext = a.vk.swapchain_extent();
    a.text.begin(cmd, ext);
    a.text.set_scale(1.0f);
    Scene& s = a.scenes[a.scene];
    float y = 10.0f;
    auto line = [&](const char* txt, float r = 1, float g = 1, float b = 1) {
        a.text.draw_text(10.0f, y, txt, r, g, b, 1.0f);
        y += 18.0f;
    };
    char buf[256];
    line(s.name, 1.0f, 0.85f, 0.4f);
    float t = a.sky_time;
    if (a.scene == kProfile && (s.flags & 1))
        t = blend_clip_times(a.clip_a / 24.0f, 1.0f - a.clip_w, a.clip_b / 24.0f, a.clip_w);
    t -= std::floor(t);
    int hh = int(t * 24.0f), mm = int((t * 24.0f - float(hh)) * 60.0f);
    snprintf(buf, sizeof(buf), "SkyTime %.3f = %02d:%02d %s  ([ ] scrub, T play)  exposure %.2f (- =)",
             t, hh, mm, a.time_playing ? "PLAY" : "", a.exposure);
    line(buf, 0.7f, 0.9f, 1.0f);
    if (!a.hud) { a.text.end(); return; }
    y += 4.0f;
    for (size_t i = 0; i < s.params.size(); ++i) {
        const Param& p = s.params[i];
        bool sel = int(i) == a.selected_param;
        if (p.integer) snprintf(buf, sizeof(buf), "%s %-24s %d", sel ? ">" : " ", p.name, int(std::round(*p.value)));
        else           snprintf(buf, sizeof(buf), "%s %-24s %.4g", sel ? ">" : " ", p.name, *p.value);
        line(buf, sel ? 1.0f : 0.75f, sel ? 1.0f : 0.75f, sel ? 0.5f : 0.75f);
    }
    y += 4.0f;
    for (int i = 0; i < 6; ++i) {
        if (!s.toggles[i]) continue;
        bool on = (s.flags >> i) & 1;
        snprintf(buf, sizeof(buf), "[%s] %-22s %s", kToggleKeyNames[i], s.toggles[i], on ? "ON" : "off");
        line(buf, on ? 0.5f : 0.6f, on ? 1.0f : 0.6f, on ? 0.5f : 0.6f);
    }
    if (a.scene == kGrass) {
        const GrassCullStats& st = a.grass.stats();
        const char* bs[3] = {"none", "constructing (big collider)", "built (small collider)"};
        snprintf(buf, sizeof(buf), "[G] building: %s", bs[static_cast<int>(a.grass.building())]);
        line(buf, 0.9f, 0.8f, 0.6f);
        snprintf(buf, sizeof(buf), "grass %u  cells %u/%u visible  dispatch %u  candidates %u",
                 st.total_instances, st.visible_cells, st.total_cells, st.dispatches, st.candidate_instances);
        line(buf, 0.6f, 0.9f, 0.9f);
    }
    if (a.scene == kProfile) {
        snprintf(buf, sizeof(buf), "Clip A %05.2fh x %.2f + Clip B %05.2fh x %.2f = %.3f",
                 a.clip_a, 1.0f - a.clip_w, a.clip_b, a.clip_w,
                 blend_clip_times(a.clip_a / 24.0f, 1.0f - a.clip_w, a.clip_b / 24.0f, a.clip_w));
        line(buf, 0.9f, 0.8f, 0.6f);
        line("tracks: Sunlight Color / Rayleigh Color / Mie Color / Star Intensity", 0.6f, 0.6f, 0.6f);
    }
    y += 4.0f;
    line("1-9 scene | Up/Down select, Left/Right adjust (Shift x4), PgUp/PgDn int | Z X C V N M toggles", 0.55f, 0.55f, 0.55f);
    line("RMB drag look, WASD/QE move (Shift fast) | B rebake | R reset | H hud | Esc quit", 0.55f, 0.55f, 0.55f);
    a.text.end();
}

// ---------------------------------------------------------------------------
// 描画
// ---------------------------------------------------------------------------
void fullscreen(VkCommandBuffer cmd, VkPipeline pipe, VkPipelineLayout layout, VkDescriptorSet set) {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &set, 0, nullptr);
    vkCmdDraw(cmd, 3, 1, 0, 0);
}

void record_frame(App& a, VkCommandBuffer cmd, uint32_t image_idx, uint32_t flight, const Mat4& vp) {
    VkDescriptorSet set0 = a.set0[flight];
    Scene& s = a.scenes[a.scene];

    // ---- ベイク (起動時 / B キー) ----
    if (a.bake_pending) {
        a.cloud_bake.begin(cmd); fullscreen(cmd, a.pipe_cloud_bake, a.layout_fs, set0); a.cloud_bake.end(cmd);
        a.probe_bake.begin(cmd); fullscreen(cmd, a.pipe_probe_bake, a.layout_fs, set0); a.probe_bake.end(cmd);
        a.local_bake.begin(cmd); fullscreen(cmd, a.pipe_local_bake, a.layout_fs, set0); a.local_bake.end(cmd);
        a.bake_pending = false;
        printf("[unite_bg] baked: cloud SixWay 512x256 x2, sky occlusion probes 17x4x13, local probe 6 faces x3\n");
    }
    // ---- ライトシャフト: シーン (色 + 距離) を先にオフスクリーンへ ----
    if (a.scene == kShaft) {
        a.shaft_target.begin(cmd);
        fullscreen(cmd, a.pipe_shaft_scene, a.layout_fs, set0);
        a.shaft_target.end(cmd);
    }
    // ---- 草: カリング (レンダーパスの外) ----
    if (a.scene == kGrass) {
        a.grass.record_compute(cmd, flight, vp, a.cam_pos, s.p[0][1], s.p[0][2], (s.flags & 1) != 0,
                               a.pipe_grass_env, a.pipe_grass_cull, a.layout_grass_comp, set0);
    }

    VkExtent2D ext = a.fb_extent;
    VkClearValue clears[2];
    clears[0].color = {{0.0f, 0.0f, 0.0f, 1.0f}};
    clears[1].depthStencil = {1.0f, 0};
    VkRenderPassBeginInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rp.renderPass = a.render_pass;
    rp.framebuffer = a.framebuffers[image_idx];
    rp.renderArea = {{0, 0}, ext};
    rp.clearValueCount = 2;
    rp.pClearValues = clears;
    vkCmdBeginRenderPass(cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);
    VkViewport v{0.0f, 0.0f, float(ext.width), float(ext.height), 0.0f, 1.0f};
    VkRect2D sc{{0, 0}, ext};
    vkCmdSetViewport(cmd, 0, 1, &v);
    vkCmdSetScissor(cmd, 0, 1, &sc);

    if (a.scene == kGrass) {
        fullscreen(cmd, a.pipe_grass_ground, a.layout_fs, set0);
        a.grass.record_draw(cmd, flight, a.pipe_grass, a.layout_grass_gfx, set0);
    } else {
        fullscreen(cmd, s.pipeline, a.layout_fs, set0);
    }
    draw_hud(a, cmd);
    vkCmdEndRenderPass(cmd);
}

struct Args { uint32_t frames = 0; int scene = 0; };
Args parse_args(int argc, char** argv) {
    Args r;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--frames") && i + 1 < argc) r.frames = uint32_t(std::atoi(argv[++i]));
        else if (!std::strcmp(argv[i], "--scene") && i + 1 < argc) r.scene = std::clamp(std::atoi(argv[++i]) - 1, 0, kSceneCount - 1);
    }
    return r;
}

} // namespace

int main(int argc, char** argv) {
    Args args = parse_args(argc, argv);
    printf("=== Pictor Unite BG Demo (hololive Dreams talk study) ===\n");

    static App app;
    App& a = app;

    pictor::GlfwWindowConfig wc;
    wc.width = 1280; wc.height = 720;
    wc.title = "Pictor - Unite BG Demo";
    wc.vsync = true;
    if (!a.surface.create(wc)) { fprintf(stderr, "window failed\n"); return 1; }

    pictor::VulkanContextConfig vc;
    vc.app_name = "Pictor Unite BG";
#ifndef NDEBUG
    vc.validation = true;
#endif
    if (!a.vk.initialize(&a.surface, vc)) {
        fprintf(stderr, "Vulkan init failed: %s\n", a.vk.init_result().detail.c_str());
        a.surface.destroy();
        return 1;
    }

    init_scenes(a);
    a.grass.generate();
    printf("[unite_bg] grass placement: %u clumps\n", a.grass.stats().total_instances);

    if (!create_render_pass(a) || !create_descriptors(a) || !create_main_targets(a) || !create_pipelines(a) ||
        !a.grass.create_gpu(a.vk, a.set1_layout, a.pool, a.vk.frames_in_flight()) ||
        !a.text.initialize(a.vk, "shaders", a.render_pass)) {
        fprintf(stderr, "[unite_bg] initialization failed (shaders/unite_bg/*.spv が見つかるか確認)\n");
        return 1;
    }
    enter_scene(a, args.scene, false);

    auto t0 = std::chrono::steady_clock::now();
    auto prev = t0;
    uint32_t frame = 0;
    while (!a.surface.should_close()) {
        a.surface.poll_events();
        auto now = std::chrono::steady_clock::now();
        float dt = std::min(std::chrono::duration<float>(now - prev).count(), 0.1f);
        prev = now;
        float time = std::chrono::duration<float>(now - t0).count();

        handle_input(a, dt);
        update_scene_objects(a, time, dt);

        const uint32_t image_idx = a.vk.acquire_next_image();
        if (image_idx == UINT32_MAX) continue;
        if (a.vk.swapchain_extent().width != a.fb_extent.width ||
            a.vk.swapchain_extent().height != a.fb_extent.height ||
            a.vk.swapchain_image_views() != a.fb_views) {
            a.vk.device_wait_idle();
            create_main_targets(a);
        }
        const uint32_t flight = a.vk.current_frame();

        FrameUBO ubo;
        fill_ubo(a, ubo, time, dt);
        std::memcpy(a.ubos[flight].mapped, &ubo, sizeof(ubo));
        Mat4 vp;
        std::memcpy(vp.m, ubo.view_proj, sizeof(vp.m));

        VkCommandBuffer cmd = a.vk.command_buffers()[image_idx];
        vkResetCommandBuffer(cmd, 0);
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        vkBeginCommandBuffer(cmd, &bi);
        record_frame(a, cmd, image_idx, flight, vp);
        vkEndCommandBuffer(cmd);

        VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSemaphore wait_sem = a.vk.image_available_semaphore();
        VkSemaphore sig_sem = a.vk.render_finished_semaphore();
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.waitSemaphoreCount = 1; si.pWaitSemaphores = &wait_sem; si.pWaitDstStageMask = &wait_stage;
        si.commandBufferCount = 1; si.pCommandBuffers = &cmd;
        si.signalSemaphoreCount = 1; si.pSignalSemaphores = &sig_sem;
        vkQueueSubmit(a.vk.graphics_queue(), 1, &si, a.vk.in_flight_fence());
        a.vk.present(image_idx);

        if (args.frames && ++frame >= args.frames) break;
    }

    // 後片付け
    a.vk.device_wait_idle();
    VkDevice dev = a.vk.device();
    a.text.shutdown();
    a.grass.destroy(dev);
    for (auto& s : a.scenes) if (s.pipeline) vkDestroyPipeline(dev, s.pipeline, nullptr);
    VkPipeline extra[] = {a.pipe_grass_ground, a.pipe_grass, a.pipe_grass_env, a.pipe_grass_cull,
                          a.pipe_shaft_scene, a.pipe_cloud_bake, a.pipe_probe_bake, a.pipe_local_bake};
    for (VkPipeline p : extra) if (p) vkDestroyPipeline(dev, p, nullptr);
    vkDestroyPipelineLayout(dev, a.layout_fs, nullptr);
    vkDestroyPipelineLayout(dev, a.layout_grass_gfx, nullptr);
    vkDestroyPipelineLayout(dev, a.layout_grass_comp, nullptr);
    a.cloud_bake.destroy(dev); a.probe_bake.destroy(dev); a.local_bake.destroy(dev); a.shaft_target.destroy(dev);
    for (auto fb : a.framebuffers) vkDestroyFramebuffer(dev, fb, nullptr);
    a.depth.destroy(dev);
    for (auto& u : a.ubos) u.destroy(dev);
    vkDestroySampler(dev, a.sampler, nullptr);
    vkDestroyDescriptorPool(dev, a.pool, nullptr);
    vkDestroyDescriptorSetLayout(dev, a.set0_layout, nullptr);
    vkDestroyDescriptorSetLayout(dev, a.set1_layout, nullptr);
    vkDestroyRenderPass(dev, a.render_pass, nullptr);
    a.vk.shutdown();
    a.surface.destroy();
    return 0;
}
