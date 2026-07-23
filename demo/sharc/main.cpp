/// Pictor SHaRC 拡張デモ — D1 Roughness Ladder / D2 逆光透過
///
/// spec: pictor-sharc-ext-design.md §6
///   D1 (既定): roughness スイープ球 ×8 + 床 + 近接光源
///     検証対象: 鏡面パララックス / 近接光源のハイライト肥大
///   D2 (引数に PLY): スキャンモデル + 背面光源 (逆光)
///     検証対象: SSS の screen-space 破綻ケース (画面外光源動線)
///     例: pictor_sharc_demo ../demo/assets/sharc/dragon/dragon_recon/dragon_vrip_res2.ply
///   Hero (引数に OBJ): Bistro クレイレンダ (MTL Kd/Ns のみ、 テクスチャ未対応)
///     例: pictor_sharc_demo ../demo/assets/sharc/bistro/Exterior/exterior.obj
///
/// 構成 (decoupled shading の最小配線):
///   1. CPU が低解像度グリッドの一次レイを解析交差 (球 / 床 / メッシュ BVH)
///      → SharcRay + SharcShadeRequest を mapped 直書き
///   2. SharcGpuExecutor が 4 パス (march/compact/update/resolve) を dispatch
///   3. 解決済み放射輝度を読み戻し → トーンマップ → Texture2DRenderer で表示
///
/// Controls:
///   Mouse drag — Orbit camera / Scroll — Zoom
///   L          — 光源アニメーション一時停止
///   ESC        — 終了
///
/// Build target: pictor_sharc_demo

#include "pictor/gi/sharc_executor.h"
#include "pictor/surface/vulkan_context.h"
#include "pictor/surface/glfw_surface_provider.h"
#include "texture2d_renderer.h"
#include "mesh_bvh.h"
#include "obj_mesh.h"
#include "ply_mesh.h"
#include <GLFW/glfw3.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace pictor;

namespace {

// ============================================================
// 最小ベクトル演算 (デモローカル)
// ============================================================

struct Vec3 {
    float x = 0, y = 0, z = 0;
};

Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3 operator*(Vec3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
float dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
float len(Vec3 a) { return std::sqrt(dot(a, a)); }
Vec3 norm(Vec3 a) { float l = len(a); return (l > 1e-8f) ? a * (1.0f / l) : Vec3{0, 1, 0}; }
Vec3 cross(Vec3 a, Vec3 b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

// ============================================================
// プロシージャルシーン (D1: roughness ladder)
// ============================================================

struct Sphere {
    Vec3  center;
    float radius;
    Vec3  albedo;
    float roughness;
    float mfp;         // 0 = SSS 無効
};

constexpr int   kSphereCount = 8;
constexpr float kFloorY      = 0.0f;
constexpr int   kRenderW     = 320;
constexpr int   kRenderH     = 180;

// シーンモードで変わる範囲 (D1/D2 = 40m、 Bistro = 250m)
float g_ray_tmax = 40.0f;

std::vector<Sphere> build_scene() {
    std::vector<Sphere> s;
    for (int i = 0; i < kSphereCount; ++i) {
        Sphere sp;
        sp.center    = {(-3.5f + i) * 1.6f, 1.0f, 0.0f};
        sp.radius    = 0.7f;
        sp.albedo    = {0.85f, 0.83f, 0.80f};
        sp.roughness = 0.05f + 0.9f * static_cast<float>(i) /
                       static_cast<float>(kSphereCount - 1);
        sp.mfp       = (i == 0) ? 0.06f : 0.0f;   // 左端のみ SSS (層1 経路確認)
        s.push_back(sp);
    }
    return s;
}

struct HitInfo {
    float t = -1.0f;
    Vec3  pos;
    Vec3  normal;
    Vec3  albedo;
    float roughness = 1.0f;
    float mfp = 0.0f;
};

/// メッシュシーン (D2: PLY / Bistro: OBJ)。
struct MeshScene {
    sharc_demo::PlyMesh mesh;
    sharc_demo::MeshBvh bvh;
    bool active    = false;
    bool use_floor = false;   ///< PLY (単体モデル) のみチェッカー床を敷く
};

// D2: 翡翠風 SSS マテリアル (MFP はワールドスケール、 fit 後 3m モデル基準)
constexpr float kMeshMfp       = 0.08f;
constexpr float kMeshRoughness = 0.35f;
const Vec3      kMeshAlbedo{0.35f, 0.68f, 0.45f};

bool ray_sphere(Vec3 ro, Vec3 rd, const Sphere& s, float& t) {
    Vec3 oc = ro - s.center;
    float b = dot(oc, rd);
    float c = dot(oc, oc) - s.radius * s.radius;
    float disc = b * b - c;
    if (disc < 0.0f) return false;
    float sq = std::sqrt(disc);
    float t0 = -b - sq;
    if (t0 > 1e-3f) { t = t0; return true; }
    float t1 = -b + sq;
    if (t1 > 1e-3f) { t = t1; return true; }
    return false;
}

HitInfo trace_scene(Vec3 ro, Vec3 rd, const std::vector<Sphere>& scene,
                    const MeshScene& mesh_scene) {
    HitInfo hit;
    float best = g_ray_tmax;
    if (mesh_scene.active) {
        const float o[3] = {ro.x, ro.y, ro.z};
        const float d[3] = {rd.x, rd.y, rd.z};
        const auto mh = mesh_scene.bvh.intersect(o, d, best);
        if (mh.valid()) {
            best = mh.t;
            hit.t = mh.t;
            hit.pos = ro + rd * mh.t;
            hit.normal = {mh.normal[0], mh.normal[1], mh.normal[2]};
            if (!mesh_scene.mesh.materials.empty()) {
                // OBJ: MTL 由来のマテリアル (Bistro クレイレンダ、 SSS なし)
                const auto& m = mesh_scene.mesh.materials
                    [mesh_scene.mesh.tri_material[mh.triangle]];
                hit.albedo = {m.albedo[0], m.albedo[1], m.albedo[2]};
                hit.roughness = m.roughness;
                hit.mfp = 0.0f;
            } else {
                // PLY: 単一 SSS マテリアル (D2 逆光透過)
                hit.albedo = kMeshAlbedo;
                hit.roughness = kMeshRoughness;
                hit.mfp = kMeshMfp;
            }
        }
    }
    for (const auto& s : scene) {
        float t;
        if (ray_sphere(ro, rd, s, t) && t < best) {
            best = t;
            hit.t = t;
            hit.pos = ro + rd * t;
            hit.normal = norm(hit.pos - s.center);
            hit.albedo = s.albedo;
            hit.roughness = s.roughness;
            hit.mfp = s.mfp;
        }
    }
    // 床 (y = kFloorY, チェッカー albedo)。 Bistro (OBJ) は実地面があるので省く
    if ((!mesh_scene.active || mesh_scene.use_floor) && rd.y < -1e-5f) {
        float t = (kFloorY - ro.y) / rd.y;
        if (t > 1e-3f && t < best) {
            hit.t = t;
            hit.pos = ro + rd * t;
            hit.normal = {0, 1, 0};
            int cx = static_cast<int>(std::floor(hit.pos.x)) & 1;
            int cz = static_cast<int>(std::floor(hit.pos.z)) & 1;
            float c = (cx == cz) ? 0.55f : 0.30f;
            hit.albedo = {c, c, c};
            hit.roughness = 0.35f;   // やや光沢のある床 — パララックス確認用
            hit.mfp = 0.0f;
        }
    }
    return hit;
}

// ============================================================
// カメラ / 入力
// ============================================================

struct OrbitState {
    float yaw = 0.35f, pitch = 0.42f, dist = 10.0f;
    float dist_min = 3.0f, dist_max = 30.0f;
    bool  dragging = false;
    double last_x = 0, last_y = 0;
    bool  light_anim = true;
};
OrbitState g_orbit;
Vec3 g_target{0.0f, 1.0f, 0.0f};

void mouse_button_cb(GLFWwindow* w, int button, int action, int) {
    if (button == GLFW_MOUSE_BUTTON_LEFT) {
        g_orbit.dragging = (action == GLFW_PRESS);
        glfwGetCursorPos(w, &g_orbit.last_x, &g_orbit.last_y);
    }
}

void cursor_pos_cb(GLFWwindow*, double x, double y) {
    if (!g_orbit.dragging) return;
    g_orbit.yaw   += static_cast<float>(x - g_orbit.last_x) * 0.005f;
    g_orbit.pitch += static_cast<float>(y - g_orbit.last_y) * 0.005f;
    g_orbit.pitch  = std::clamp(g_orbit.pitch, 0.05f, 1.35f);
    g_orbit.last_x = x;
    g_orbit.last_y = y;
}

void scroll_cb(GLFWwindow*, double, double dy) {
    g_orbit.dist = std::clamp(g_orbit.dist - static_cast<float>(dy) * 0.8f,
                              g_orbit.dist_min, g_orbit.dist_max);
}

void key_cb(GLFWwindow* w, int key, int, int action, int) {
    if (action != GLFW_PRESS) return;
    if (key == GLFW_KEY_ESCAPE) glfwSetWindowShouldClose(w, GLFW_TRUE);
    if (key == GLFW_KEY_L) g_orbit.light_anim = !g_orbit.light_anim;
}

// ============================================================
// トーンマップ (Reinhard + gamma 2.2)
// ============================================================

constexpr float kExposure = 0.25f;   // 白飛び防止 (キャッシュは HDR 蓄積)

uint8_t tonemap_channel(float v) {
    v = std::max(v * kExposure, 0.0f);
    v = v / (1.0f + v);
    v = std::pow(v, 1.0f / 2.2f);
    return static_cast<uint8_t>(std::clamp(v * 255.0f + 0.5f, 0.0f, 255.0f));
}

} // namespace

int main(int argc, char** argv) {
    // ── シーン選択: .ply = D2 (逆光透過) / .obj = Bistro / なし = D1 ──
    MeshScene mesh_scene;
    bool obj_scene = false;
    std::string title = "Pictor SHaRC D1 - Roughness Ladder";
    if (argc > 1) {
        const std::string path = argv[1];
        obj_scene = (path.size() > 4 &&
                     path.compare(path.size() - 4, 4, ".obj") == 0);
        if (obj_scene) {
            mesh_scene.mesh = sharc_demo::load_obj(path);
        } else {
            mesh_scene.mesh = sharc_demo::load_ply(path);
        }
        if (mesh_scene.mesh.empty()) {
            std::fprintf(stderr, "[init] FATAL: mesh load failed: %s\n",
                         argv[1]);
            return 1;
        }
        if (obj_scene) {
            auto& m = mesh_scene.mesh;
            // 単位系検出: extent が 1km 超なら cm 単位とみなして m へ変換
            // (Bistro Exterior は extent ≈ 11,526 = cm、 実寸 ≈ 115m)。
            float extent = 0.0f;
            for (int a = 0; a < 3; ++a) {
                extent = std::max(extent, m.bounds_max[a] - m.bounds_min[a]);
            }
            if (extent > 1000.0f) {
                sharc_demo::scale_mesh(m, 0.01f);
                extent *= 0.01f;
                std::fprintf(stderr, "[scene] assumed cm units, scaled x0.01\n");
            }
            // AABB はバックドロップ等の外れジオメトリで歪むため、 カメラ
            // ターゲットは頂点重心 (= ジオメトリ密度の中心 ≒ 街路部) に置く。
            double cx = 0.0, cy = 0.0, cz = 0.0;
            for (const auto& p : m.positions) {
                cx += p[0]; cy += p[1]; cz += p[2];
            }
            const double inv = 1.0 / static_cast<double>(m.positions.size());
            g_target = {static_cast<float>(cx * inv),
                        static_cast<float>(cy * inv),
                        static_cast<float>(cz * inv)};
            std::fprintf(stderr,
                         "[scene] bounds (%.1f %.1f %.1f)-(%.1f %.1f %.1f) "
                         "centroid (%.1f %.1f %.1f) extent %.1f\n",
                         m.bounds_min[0], m.bounds_min[1], m.bounds_min[2],
                         m.bounds_max[0], m.bounds_max[1], m.bounds_max[2],
                         g_target.x, g_target.y, g_target.z, extent);
            g_ray_tmax        = 400.0f;
            g_orbit.dist      = 30.0f;
            g_orbit.dist_min  = 5.0f;
            g_orbit.dist_max  = 200.0f;
            g_orbit.pitch     = 0.35f;
            title = "Pictor SHaRC Hero - Bistro (clay)";
        } else {
            sharc_demo::fit_mesh(mesh_scene.mesh, 3.0f);
            mesh_scene.use_floor = true;
            title = "Pictor SHaRC D2 - Backlit Transmission";
        }
        mesh_scene.bvh.build(mesh_scene.mesh);
        mesh_scene.active = true;
    }
    std::printf("=== %s ===\n", title.c_str());

    // ── window + Vulkan ──
    GlfwSurfaceProvider surface;
    GlfwWindowConfig win_cfg;
    win_cfg.width  = 1280;
    win_cfg.height = 720;
    win_cfg.title  = title;
    if (!surface.create(win_cfg)) {
        std::fprintf(stderr, "[init] FATAL: window creation failed\n");
        return 1;
    }
    VulkanContext vk;
    VulkanContextConfig vk_cfg;
    vk_cfg.app_name = "pictor_sharc_demo";
    if (!vk.initialize(&surface, vk_cfg)) {
        std::fprintf(stderr, "[init] FATAL: Vulkan context init failed\n");
        surface.destroy();
        return 1;
    }

    // ── SHaRC executor ──
    SharcConfig cfg;
    cfg.max_rays = kRenderW * kRenderH;
    if (obj_scene) {
        // 街路スケールは可視セル数が桁違い — テーブルを 262k slots へ拡張
        // (線形走査の飽和 = 挿入失敗による黒領域を防ぐ)
        cfg.table_size = 1u << 18;
    }
    SharcGpuExecutor sharc;
    if (!sharc.initialize(vk, "shaders", cfg)) {
        std::fprintf(stderr, "[init] FATAL: SHaRC executor init failed\n");
        vk.shutdown();
        surface.destroy();
        return 1;
    }

    // ── 表示用 (texture2d デモの再利用) ──
    Texture2DRenderer tex;
    if (!tex.initialize(vk, "shaders")) {
        std::fprintf(stderr, "[init] FATAL: Texture2DRenderer init failed\n");
        sharc.shutdown();
        vk.shutdown();
        surface.destroy();
        return 1;
    }

    // ── compute 用コマンドバッファ ──
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool        = vk.command_pool();
    cai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    VkCommandBuffer compute_cmd = VK_NULL_HANDLE;
    if (vkAllocateCommandBuffers(vk.device(), &cai, &compute_cmd) != VK_SUCCESS) {
        std::fprintf(stderr, "[init] FATAL: command buffer allocation failed\n");
        return 1;
    }

    GLFWwindow* win = surface.glfw_window();
    glfwSetMouseButtonCallback(win, mouse_button_cb);
    glfwSetCursorPosCallback(win, cursor_pos_cb);
    glfwSetScrollCallback(win, scroll_cb);
    glfwSetKeyCallback(win, key_cb);

    // D2 はメッシュのみ (球ラダーは D1 専用)
    const auto scene = mesh_scene.active ? std::vector<Sphere>{}
                                         : build_scene();
    std::vector<uint8_t> rgba(kRenderW * kRenderH * 4);

    std::printf("[loop] %ux%u rays, %d spheres. Drag=orbit Scroll=zoom "
                "L=light-anim ESC=quit\n", kRenderW, kRenderH, kSphereCount);

    const auto t_start = std::chrono::steady_clock::now();
    uint64_t frame = 0;
    float light_time = 0.0f;
    auto t_prev = t_start;

    while (!surface.should_close()) {
        surface.poll_events();

        const auto t_now = std::chrono::steady_clock::now();
        const float dt = std::chrono::duration<float>(t_now - t_prev).count();
        t_prev = t_now;
        if (g_orbit.light_anim) light_time += dt;

        // ── カメラ (orbit) ──
        const Vec3 target = g_target;
        const Vec3 eye{
            target.x + g_orbit.dist * std::cos(g_orbit.pitch) * std::sin(g_orbit.yaw),
            target.y + g_orbit.dist * std::sin(g_orbit.pitch),
            target.z + g_orbit.dist * std::cos(g_orbit.pitch) * std::cos(g_orbit.yaw)};
        const Vec3 fwd   = norm(target - eye);
        const Vec3 right = norm(cross(fwd, Vec3{0, 1, 0}));
        const Vec3 up    = cross(right, fwd);
        const float fov_scale = std::tan(0.5f * 60.0f * 3.14159265f / 180.0f);
        const float aspect = static_cast<float>(kRenderW) /
                             static_cast<float>(kRenderH);

        sharc.begin_frame(float3{eye.x, eye.y, eye.z});
        auto* lights = sharc.lights_mapped();
        if (obj_scene) {
            // ── Bistro: 高所の太陽light + 街路を漂う暖色光源 (ターゲット基準) ──
            lights[0] = SharcLightGpu{
                {target.x + 60.0f, target.y + 80.0f, target.z - 40.0f, 1.0f},
                {1.0f, 0.92f, 0.8f, 9000.0f}};                       // 太陽風
            const float lx = std::sin(light_time * 0.3f) * 8.0f;
            lights[1] = SharcLightGpu{
                {target.x + lx, target.y + 4.0f, target.z + 5.0f, 0.3f},
                {1.0f, 0.7f, 0.4f, 300.0f}};                         // 街灯風
        } else if (mesh_scene.active) {
            // ── D2: 背面光源 (逆光)。 カメラの反対側を横断し、 モデル越しの
            //    透過 (SSS) を見る。 画面外に出る動線も含む ──
            const Vec3 behind = norm(target - eye);
            const float lx = std::sin(light_time * 0.4f) * 2.5f;
            lights[0] = SharcLightGpu{
                {target.x + behind.x * 4.0f + lx, 1.8f,
                 target.z + behind.z * 4.0f, 0.2f},
                {1.0f, 0.75f, 0.5f, 90.0f}};                       // 逆光・暖色
            lights[1] = SharcLightGpu{{eye.x, eye.y + 2.0f, eye.z, 0.5f},
                                      {0.3f, 0.35f, 0.5f, 15.0f}}; // 弱い前面フィル
        } else {
            // ── D1: 球列の直上を横断する近接光源 (ハイライト肥大検証) ──
            const float lx = std::sin(light_time * 0.6f) * 5.5f;
            lights[0] = SharcLightGpu{{lx, 2.6f, 1.2f, 0.15f},
                                      {1.0f, 0.85f, 0.6f, 60.0f}};  // 近接・暖色
            lights[1] = SharcLightGpu{{0.0f, 8.0f, 6.0f, 0.5f},
                                      {0.4f, 0.5f, 0.8f, 120.0f}};  // フィル・寒色
        }

        // ── CPU 一次レイ: 交差 → ray + shade request 直書き ──
        auto* rays  = sharc.rays_mapped();
        auto* shade = sharc.shade_requests_mapped();
        for (int y = 0; y < kRenderH; ++y) {
            for (int x = 0; x < kRenderW; ++x) {
                const int idx = y * kRenderW + x;
                const float u = (2.0f * (x + 0.5f) / kRenderW - 1.0f) * fov_scale * aspect;
                const float v = (1.0f - 2.0f * (y + 0.5f) / kRenderH) * fov_scale;
                const Vec3 rd = norm(fwd + right * u + up * v);
                const HitInfo hit = trace_scene(eye, rd, scene, mesh_scene);
                const float tmax = (hit.t > 0.0f) ? hit.t : g_ray_tmax;

                rays[idx] = SharcRayGpu{{eye.x, eye.y, eye.z, 0.0f},
                                        {rd.x, rd.y, rd.z, tmax}};
                if (hit.t > 0.0f) {
                    const Vec3 view = norm(eye - hit.pos);
                    shade[idx] = SharcShadeRequestGpu{
                        {hit.pos.x, hit.pos.y, hit.pos.z, hit.roughness},
                        {hit.normal.x, hit.normal.y, hit.normal.z, hit.mfp},
                        {hit.albedo.x, hit.albedo.y, hit.albedo.z, 0.0f},
                        {view.x, view.y, view.z, 0.0f}};
                } else {
                    // ミス: albedo 0 → 出力 0 (空)。 march はセルを温める。
                    shade[idx] = SharcShadeRequestGpu{
                        {0.0f, 0.0f, 0.0f, 1.0f},
                        {0.0f, 1.0f, 0.0f, 0.0f},
                        {0.0f, 0.0f, 0.0f, 0.0f},
                        {0.0f, 1.0f, 0.0f, 0.0f}};
                }
            }
        }
        sharc.set_counts(kRenderW * kRenderH, 2);

        // ── 4 パス dispatch (同期実行 — デモは単純さ優先) ──
        vkResetCommandBuffer(compute_cmd, 0);
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        vkBeginCommandBuffer(compute_cmd, &bi);
        sharc.record(compute_cmd);
        vkEndCommandBuffer(compute_cmd);

        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1;
        si.pCommandBuffers    = &compute_cmd;
        vkQueueSubmit(vk.graphics_queue(), 1, &si, VK_NULL_HANDLE);
        vkQueueWaitIdle(vk.graphics_queue());

        // ── 読み戻し → トーンマップ ──
        const float* out = sharc.output_mapped();
        for (int i = 0; i < kRenderW * kRenderH; ++i) {
            rgba[i * 4 + 0] = tonemap_channel(out[i * 4 + 0]);
            rgba[i * 4 + 1] = tonemap_channel(out[i * 4 + 1]);
            rgba[i * 4 + 2] = tonemap_channel(out[i * 4 + 2]);
            rgba[i * 4 + 3] = 255;
        }
        vk.device_wait_idle();
        tex.upload_texture(rgba.data(), kRenderW, kRenderH);

        // ── 表示 (フルスクリーン quad) ──
        const uint32_t image_idx = vk.acquire_next_image();
        if (image_idx == UINT32_MAX) continue;

        auto cmd = vk.command_buffers()[image_idx];
        vkResetCommandBuffer(cmd, 0);
        VkCommandBufferBeginInfo begin_info{
            VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        vkBeginCommandBuffer(cmd, &begin_info);

        const auto ext = vk.swapchain_extent();
        VkClearValue clear = {{{0.05f, 0.05f, 0.08f, 1.0f}}};
        VkRenderPassBeginInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        rp.renderPass      = vk.default_render_pass();
        rp.framebuffer     = vk.framebuffers()[image_idx];
        rp.renderArea      = {{0, 0}, ext};
        rp.clearValueCount = 1;
        rp.pClearValues    = &clear;
        vkCmdBeginRenderPass(cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);

        Texture2DPushConstants pc{};
        // 正射影でフルスクリーンに引き伸ばす
        const float w = static_cast<float>(ext.width);
        const float h = static_cast<float>(ext.height);
        std::memset(pc.projection, 0, sizeof(pc.projection));
        pc.projection[0]  = 2.0f / w;
        pc.projection[5]  = 2.0f / h;
        pc.projection[10] = 1.0f;
        pc.projection[12] = -1.0f;
        pc.projection[13] = -1.0f;
        pc.projection[15] = 1.0f;
        std::memset(pc.model, 0, sizeof(pc.model));
        pc.model[0]  = w;
        pc.model[5]  = h;
        pc.model[10] = 1.0f;
        pc.model[12] = w * 0.5f;
        pc.model[13] = h * 0.5f;
        pc.model[15] = 1.0f;
        pc.tint[0] = pc.tint[1] = pc.tint[2] = pc.tint[3] = 1.0f;
        tex.render(cmd, ext, pc);

        vkCmdEndRenderPass(cmd);
        vkEndCommandBuffer(cmd);

        VkPipelineStageFlags wait_stage =
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSemaphore wait_sem = vk.image_available_semaphore();
        VkSemaphore sig_sem  = vk.render_finished_semaphore();
        VkSubmitInfo present_si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        present_si.waitSemaphoreCount   = 1;
        present_si.pWaitSemaphores      = &wait_sem;
        present_si.pWaitDstStageMask    = &wait_stage;
        present_si.commandBufferCount   = 1;
        present_si.pCommandBuffers      = &cmd;
        present_si.signalSemaphoreCount = 1;
        present_si.pSignalSemaphores    = &sig_sem;
        vkQueueSubmit(vk.graphics_queue(), 1, &present_si,
                      vk.in_flight_fence());
        vk.present(image_idx);

        ++frame;
        if (frame % 120 == 0) {
            std::printf("[loop] frame %llu: %u requested cells\n",
                        static_cast<unsigned long long>(frame),
                        sharc.request_count());
        }
    }

    vk.device_wait_idle();
    tex.shutdown();
    sharc.shutdown();
    vk.shutdown();
    surface.destroy();
    std::printf("[exit] done (%llu frames)\n",
                static_cast<unsigned long long>(frame));
    return 0;
}
