/// Pictor SHaRC 拡張デモ — D1 Roughness Ladder / D2 逆光透過
///
/// spec: pictor-sharc-ext-design.md §6
///   D1 (既定): roughness スイープ球 ×8 + 床 + 近接光源
///     検証対象: 鏡面パララックス / 近接光源のハイライト肥大
///   D2 (引数に PLY): スキャンモデル + 背面光源 (逆光)
///     検証対象: SSS の screen-space 破綻ケース (画面外光源動線)
///     例: pictor_sharc_demo ../demo/assets/sharc/dragon/dragon_recon/dragon_vrip_res2.ply
///   Hero (引数に OBJ): Bistro クレイレンダ (MTL Kd/Ns のみ、 テクスチャ未対応)
///     多灯 (夕日 + 街灯列 + アクセント ~16 灯) + 葉の SSS (マテリアル名判定)
///     第 2 引数以降は複数プロップ (.obj = 人物 head 肌 SSS /
///     .ply = スキャン像 翡翠 SSS)。 地面レイキャストで街路へ接地
///     例: pictor_sharc_demo ../demo/assets/sharc/bistro/Exterior/exterior.obj \
///         ../demo/assets/sharc/lpshead/head.OBJ \
///         ../demo/assets/sharc/dragon/dragon_recon/dragon_vrip_res2.ply
///
/// 構成 (decoupled shading の最小配線):
///   1. CPU が低解像度グリッドの一次レイを解析交差 (球 / 床 / メッシュ BVH)
///      → SharcRay + SharcShadeRequest を mapped 直書き
///   2. SharcGpuExecutor が 4 パス (march/compact/update/resolve) を dispatch
///   3. present が resolve 出力 SSBO を fragment 直読み → トーンマップ表示
///      (CPU readback / テクスチャ再アップロードなしの全 GPU 経路)
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
#include "mesh_bvh.h"
#include "obj_mesh.h"
#include "ply_mesh.h"
#include "bloom_pipeline.h"
#include "gbuffer_renderer.h"
#include "present_renderer.h"
#include "texture_atlas.h"
#include <GLFW/glfw3.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <thread>
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
constexpr int   kRenderW     = 1280;
constexpr int   kRenderH     = 720;

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

/// メッシュ 1 体 (シーン本体 or 配置プロップ)。
struct MeshInstance {
    sharc_demo::PlyMesh mesh;
    sharc_demo::MeshBvh bvh;
    // MTL を持たないメッシュ (PLY / プロップ) 用の単一マテリアル
    bool  material_override = false;
    Vec3  ov_albedo{0.75f, 0.75f, 0.75f};
    float ov_roughness = 0.5f;
    float ov_mfp = 0.0f;
};

/// メッシュシーン (D2: PLY / Bistro: OBJ + プロップ)。
struct MeshScene {
    std::vector<std::unique_ptr<MeshInstance>> instances;
    bool active    = false;
    bool use_floor = false;   ///< PLY (単体モデル) のみチェッカー床を敷く
};

// D2: 翡翠風 SSS マテリアル (MFP はワールドスケール、 fit 後 3m モデル基準)
constexpr float kMeshMfp       = 0.08f;
constexpr float kMeshRoughness = 0.35f;
const Vec3      kMeshAlbedo{0.35f, 0.68f, 0.45f};

// 人物プロップ (Lee Perry-Smith head): 肌の SSS
constexpr float kSkinMfp       = 0.02f;
constexpr float kSkinRoughness = 0.65f;   // 鏡面優勢だと肌がクロム調に見える
const Vec3      kSkinAlbedo{0.80f, 0.58f, 0.47f};

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
    const float o[3] = {ro.x, ro.y, ro.z};
    const float d[3] = {rd.x, rd.y, rd.z};
    for (const auto& inst : mesh_scene.instances) {
        const auto mh = inst->bvh.intersect(o, d, best);
        if (!mh.valid()) continue;
        best = mh.t;
        hit.t = mh.t;
        hit.pos = ro + rd * mh.t;
        hit.normal = {mh.normal[0], mh.normal[1], mh.normal[2]};
        if (inst->material_override || inst->mesh.materials.empty()) {
            // PLY / プロップ: 単一マテリアル (D2 翡翠 / 肌など)
            hit.albedo = inst->ov_albedo;
            hit.roughness = inst->ov_roughness;
            hit.mfp = inst->ov_mfp;
        } else {
            // OBJ: MTL 由来 (Bistro。 葉は tag_foliage が MFP 付与済み)
            const auto& m = inst->mesh.materials
                [inst->mesh.tri_material[mh.triangle]];
            hit.albedo = {m.albedo[0], m.albedo[1], m.albedo[2]};
            hit.roughness = m.roughness;
            hit.mfp = m.mfp;
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
Vec3 g_prop_pos{};
Vec3 g_prop_back{0.0f, 0.0f, -1.0f};   // head からカメラと反対向き (逆光方向)
bool g_has_prop = false;
Vec3 g_scene_target{0.0f, 1.0f, 0.0f}; // B キー復帰用 (シーン既定ターゲット)
float g_scene_dist = 10.0f;
bool  g_albedo_view = false;           // T: アルベド素通し (リファレンス比較)
float g_fov_deg = 60.0f;               // F/G で増減
bool  g_hybrid = true;                 // Y: ハイブリッド/フルレイ切替 (A/B)
Vec3  g_scene_center{0.0f, 0.0f, 0.0f};  // 太陽 ortho の中心 (シーン重心)
float g_scene_extent = 20.0f;            // 同 half-extent 算出用
std::vector<sharc_demo::EmissiveGroup> g_emissives;  // 街灯/提灯 (m 単位)

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
    // カメラプリセット (検証の再現性用): H = プロップ接写 / B = シーン俯瞰
    if (key == GLFW_KEY_H && g_has_prop) {
        g_target = g_prop_pos;
        g_orbit.dist  = 1.6f;
        g_orbit.pitch = 0.12f;
        // 初期カメラ側 (= -g_prop_back) から見る
        g_orbit.yaw = std::atan2(-g_prop_back.x, -g_prop_back.z);
        std::printf("[cam] preset: prop close-up\n");
    }
    if (key == GLFW_KEY_B) {
        // 強めの見下ろしで建物壁面への埋没を避ける
        g_target = g_scene_target;
        g_orbit.dist  = g_scene_dist * 1.5f;
        g_orbit.pitch = 0.55f;
        std::printf("[cam] preset: scene overview\n");
    }
    if (key == GLFW_KEY_T) {
        g_albedo_view = !g_albedo_view;
        std::printf("[cam] albedo view: %s\n", g_albedo_view ? "on" : "off");
    }
    if (key == GLFW_KEY_Y) {
        // 120fps 試算の A/B 比較: ラスタ G-buffer ⇔ compute BVH フルレイ
        g_hybrid = !g_hybrid;
        std::printf("[path] %s\n",
                    g_hybrid ? "hybrid (raster G-buffer)" : "full ray (BVH)");
    }
    if (key == GLFW_KEY_V) {
        // ORCA 公式スクリーンショット Bistro_Exterior_1.png と同構図
        // (カフェ角 Le Petit Coin、 手動合わせ込み値)
        g_target = {-0.48f, 2.08f, -0.30f};
        g_orbit.yaw   = 4.50f;
        g_orbit.pitch = 0.49f;
        g_orbit.dist  = 13.43f;
        g_fov_deg     = 75.0f;
        std::printf("[cam] preset: reference (Bistro_Exterior_1)\n");
    }
    // ── リファレンス合わせ込み用ナッジ (P で現在値をプリント) ──
    const float fwd_x = std::sin(g_orbit.yaw);
    const float fwd_z = std::cos(g_orbit.yaw);
    if (key == GLFW_KEY_LEFT)  g_orbit.yaw   -= 0.05f;
    if (key == GLFW_KEY_RIGHT) g_orbit.yaw   += 0.05f;
    if (key == GLFW_KEY_UP)    g_orbit.pitch = std::min(g_orbit.pitch + 0.03f, 1.5f);
    if (key == GLFW_KEY_DOWN)  g_orbit.pitch = std::max(g_orbit.pitch - 0.03f, 0.02f);
    if (key == GLFW_KEY_PAGE_UP)   g_orbit.dist = std::max(g_orbit.dist * 0.9f, g_orbit.dist_min);
    if (key == GLFW_KEY_PAGE_DOWN) g_orbit.dist = std::min(g_orbit.dist * 1.1f, g_orbit.dist_max);
    if (key == GLFW_KEY_I) { g_target.x -= fwd_x; g_target.z -= fwd_z; }
    if (key == GLFW_KEY_K) { g_target.x += fwd_x; g_target.z += fwd_z; }
    if (key == GLFW_KEY_J) { g_target.x -= fwd_z; g_target.z += fwd_x; }
    if (key == GLFW_KEY_M) { g_target.x += fwd_z; g_target.z -= fwd_x; }
    if (key == GLFW_KEY_U) g_target.y += 0.5f;
    if (key == GLFW_KEY_O) g_target.y -= 0.5f;
    if (key == GLFW_KEY_F) g_fov_deg = std::max(g_fov_deg - 5.0f, 20.0f);
    if (key == GLFW_KEY_G) g_fov_deg = std::min(g_fov_deg + 5.0f, 110.0f);
    if (key == GLFW_KEY_P) {
        std::printf("[cam] target=(%.2f %.2f %.2f) yaw=%.3f pitch=%.3f "
                    "dist=%.2f fov=%.0f\n",
                    g_target.x, g_target.y, g_target.z, g_orbit.yaw,
                    g_orbit.pitch, g_orbit.dist, g_fov_deg);
    }
}

// トーンマップ (輝度 Reinhard + gamma 2.2) は present 側 GPU (sharc_present.frag)
// 1.7: 多灯化で全体光量を下げた分を露出で持ち上げ、 日陰の視認性を確保
constexpr float kExposure = 1.7f;

// Bloom 合成強度 (トーンマップ前、 露出スケール済み bloom に乗算)
constexpr float kBloomStrength = 0.08f;

// 太陽方向 (to-sun)。 ライトリグ (lights[0]) とシャドウマップ ortho の
// 両方がこれを参照する — 二重定義でズレると影が割れる。
// 夕方の終わりかけ (低仰角 ~13°、 西日) — 街灯/提灯の効果を見せる時間帯
const Vec3 kSunDir{-0.78f, 0.22f, 0.32f};

// ============================================================
// ハイブリッド経路の行列 (column-major、 GLSL mat4 互換)
// ============================================================

float axis_of(const Vec3& v, int i) { return i == 0 ? v.x : i == 1 ? v.y : v.z; }

// カメラ基底 → viewProj。 compute のレイ生成式 (rd = fwd + right*u*fovScale*
// aspect + up*v*fovScale, v = 1-2*(py+.5)/H) と厳密に一致する画素対応を持つ
// (Vulkan NDC は y 下向きなので y 行を反転)。
void build_view_proj(const Vec3& eye, const Vec3& fwd, const Vec3& right,
                     const Vec3& up, float fov_scale, float aspect,
                     float* m16) {
    const float a  = 1.0f / (fov_scale * aspect);
    const float b  = 1.0f / fov_scale;
    const float zn = 0.05f, zf = 300.0f;
    const float k  = zf / (zf - zn);
    for (int c = 0; c < 3; ++c) {
        m16[c * 4 + 0] = a * axis_of(right, c);
        m16[c * 4 + 1] = -b * axis_of(up, c);
        m16[c * 4 + 2] = k * axis_of(fwd, c);
        m16[c * 4 + 3] = axis_of(fwd, c);
    }
    m16[12] = -a * dot(right, eye);
    m16[13] = b * dot(up, eye);
    m16[14] = -k * (dot(fwd, eye) + zn);
    m16[15] = -dot(fwd, eye);
}

// 太陽 ortho VP。 描画 (sharc_shadow.vert) とサンプル
// (sharc_gbuffer_resolve.comp) が同一行列を使うため座標規約は自己完結。
void build_sun_ortho(const Vec3& to_sun, const Vec3& center,
                     float half_extent, float* m16) {
    const Vec3 l  = norm(Vec3{-to_sun.x, -to_sun.y, -to_sun.z});
    const Vec3 lr = norm(cross(l, Vec3{0.0f, 1.0f, 0.0f}));
    const Vec3 lu = cross(lr, l);
    const Vec3 eye = center - l * (half_extent * 2.0f);
    const float zn = 1.0f, zf = half_extent * 4.0f + 50.0f;
    for (int c = 0; c < 3; ++c) {
        m16[c * 4 + 0] = axis_of(lr, c) / half_extent;
        m16[c * 4 + 1] = axis_of(lu, c) / half_extent;
        m16[c * 4 + 2] = axis_of(l, c) / (zf - zn);
        m16[c * 4 + 3] = 0.0f;
    }
    m16[12] = -dot(lr, eye) / half_extent;
    m16[13] = -dot(lu, eye) / half_extent;
    m16[14] = -(dot(l, eye) + zn) / (zf - zn);
    m16[15] = 1.0f;
}

} // namespace

int main(int argc, char** argv) {
    // リダイレクト時もログが即時見えるように (ハング調査の生命線)
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);
    // ── シーン選択: .ply = D2 (逆光透過) / .obj = Bistro / なし = D1 ──
    //    第 2 引数 (任意): 配置プロップ (人物 head 等、 肌 SSS で合成)
    MeshScene mesh_scene;
    bool obj_scene = false;
    std::string title = "Pictor SHaRC D1 - Roughness Ladder";
    std::string cache_path;   // <モデル>.sharccache (温間スタート用)
    if (argc > 1) {
        const std::string path = argv[1];
        cache_path = path + ".sharccache";
        obj_scene = (path.size() > 4 &&
                     path.compare(path.size() - 4, 4, ".obj") == 0);
        auto primary = std::make_unique<MeshInstance>();
        if (obj_scene) {
            primary->mesh = sharc_demo::load_obj(path);
        } else {
            primary->mesh = sharc_demo::load_ply(path);
            primary->material_override = true;
            primary->ov_albedo = kMeshAlbedo;
            primary->ov_roughness = kMeshRoughness;
            primary->ov_mfp = kMeshMfp;
        }
        if (primary->mesh.empty()) {
            std::fprintf(stderr, "[init] FATAL: mesh load failed: %s\n",
                         argv[1]);
            return 1;
        }
        if (obj_scene) {
            auto& m = primary->mesh;
            // 単位系検出: extent が 1km 超なら cm 単位とみなして m へ変換
            // (Bistro Exterior は extent ≈ 11,526 = cm、 実寸 ≈ 115m)。
            float extent = 0.0f;
            for (int a = 0; a < 3; ++a) {
                extent = std::max(extent, m.bounds_max[a] - m.bounds_min[a]);
            }
            if (extent > 1000.0f) {
                sharc_demo::scale_mesh(m, 0.01f);
                extent *= 0.01f;
                for (auto& e : m.emissive_groups) {
                    for (float& c : e.center) c *= 0.01f;
                }
                std::fprintf(stderr, "[scene] assumed cm units, scaled x0.01\n");
            }
            g_emissives = m.emissive_groups;   // ライト自動配置 (m 単位)
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
            g_orbit.dist_min  = 1.0f;
            g_orbit.dist_max  = 200.0f;
            g_orbit.pitch     = 0.35f;
            g_scene_target    = g_target;
            g_scene_dist      = 30.0f;
            g_scene_center    = g_target;   // 太陽 ortho の中心
            g_scene_extent    = extent;
            title = "Pictor SHaRC Hero - Bistro";
        } else {
            sharc_demo::fit_mesh(primary->mesh, 3.0f);
            mesh_scene.use_floor = true;
            title = "Pictor SHaRC D2 - Backlit Transmission";
        }
        primary->bvh.build(primary->mesh);
        mesh_scene.instances.push_back(std::move(primary));
        mesh_scene.active = true;

        // ── プロップ (第 2 引数以降、 複数可): 街路の地面に SSS 素材で配置 ──
        //    .obj = 人物 head (肌, 45cm) / .ply = スキャン像 (翡翠, 1.2m)。
        //    一次交差は BVH (対数スケール) なので体数を増やしてもレイあたり
        //    コストはほぼ不変 — VRAM 48B/tri のみ。 静的モデル限定
        //    (動的は BVH refit が必要、 spec §3 将来枝)。
        for (int prop_arg = 2; prop_arg < argc; ++prop_arg) {
            if (argv[prop_arg][0] == '-') continue;   // フラグ (--novsync 等)
            auto prop = std::make_unique<MeshInstance>();
            const std::string ppath = argv[prop_arg];
            const bool prop_obj =
                (ppath.size() > 4 &&
                 (ppath.compare(ppath.size() - 4, 4, ".obj") == 0 ||
                  ppath.compare(ppath.size() - 4, 4, ".OBJ") == 0));
            prop->mesh = prop_obj ? sharc_demo::load_obj(ppath)
                                  : sharc_demo::load_ply(ppath);
            if (prop->mesh.empty()) {
                std::fprintf(stderr, "[init] FATAL: prop load failed: %s\n",
                             argv[prop_arg]);
                return 1;
            }
            const bool is_head = prop_obj;
            const float target_extent = is_head ? 0.45f : 1.2f;
            const float eye_height    = is_head ? 1.55f : 0.0f;
            sharc_demo::fit_mesh(prop->mesh, target_extent);

            const Vec3 eye0{
                g_target.x + g_orbit.dist * std::cos(g_orbit.pitch) *
                                 std::sin(g_orbit.yaw),
                g_target.y + g_orbit.dist * std::sin(g_orbit.pitch),
                g_target.z + g_orbit.dist * std::cos(g_orbit.pitch) *
                                 std::cos(g_orbit.yaw)};
            const Vec3 toward = norm(eye0 - g_target);
            // ターゲット周囲をリング状に探査し、 下向きレイキャストが
            // 最も低い地面 (= 屋根ではなく街路) に当たる点へ接地する。
            // 体ごとに開始角をずらして重なりを避ける。
            Vec3 prop_pos = g_target + toward * 8.0f;
            {
                const auto& scene_mesh = mesh_scene.instances[0]->mesh;
                const float top = scene_mesh.bounds_max[1] + 1.0f;
                const float rd[3] = {0.0f, -1.0f, 0.0f};
                const float base_ang = std::atan2(toward.x, toward.z)
                                     + static_cast<float>(prop_arg - 2) * 0.6f;
                float best_ground = 1e9f;
                Vec3 best_xz = prop_pos;
                for (int i = 0; i < 12; ++i) {
                    const float ang = base_ang + static_cast<float>(i) *
                                      (2.0f * 3.14159265f / 12.0f);
                    for (const float r : {5.0f, 8.0f, 11.0f}) {
                        const float cx = g_target.x + std::sin(ang) * r;
                        const float cz = g_target.z + std::cos(ang) * r;
                        const float ro[3] = {cx, top, cz};
                        const auto gh = mesh_scene.instances[0]->bvh.intersect(
                            ro, rd, 1e5f);
                        if (!gh.valid()) continue;
                        const float gy = top - gh.t;
                        if (gy < best_ground) {
                            best_ground = gy;
                            best_xz = {cx, 0.0f, cz};
                        }
                    }
                }
                if (best_ground > 1e8f) best_ground = 0.0f;   // 全ミス時
                prop_pos = {best_xz.x, best_ground + eye_height, best_xz.z};
            }
            for (auto& p : prop->mesh.positions) {
                p[0] += prop_pos.x;
                p[1] += prop_pos.y;
                p[2] += prop_pos.z;
            }
            sharc_demo::finalize_mesh(prop->mesh);
            if (is_head && !prop->mesh.materials.empty()) {
                // head: map_Kd (肌テクスチャ) は残し、 SSS/roughness だけ上書き
                for (auto& m : prop->mesh.materials) {
                    m.mfp       = kSkinMfp;
                    m.roughness = kSkinRoughness;
                }
            } else {
                prop->mesh.materials.clear();
                prop->material_override = true;
                prop->ov_albedo    = kMeshAlbedo;      // 翡翠 (D2 と同素材)
                prop->ov_roughness = kMeshRoughness;
                prop->ov_mfp       = kMeshMfp;
            }
            prop->bvh.build(prop->mesh);
            std::fprintf(stderr, "[scene] prop %d at (%.1f %.1f %.1f)\n",
                         prop_arg - 1, prop_pos.x, prop_pos.y, prop_pos.z);
            if (!g_has_prop) {
                // カメラプリセット H は最初のプロップを向く
                g_prop_pos = prop_pos;
                g_prop_back = norm(Vec3{-toward.x, 0.0f, -toward.z});
                g_has_prop = true;
            }
            mesh_scene.instances.push_back(std::move(prop));
        }
    }
    std::printf("=== %s ===\n", title.c_str());

    // ── window + Vulkan ──
    GlfwSurfaceProvider surface;
    GlfwWindowConfig win_cfg;
    win_cfg.width  = 1280;
    win_cfg.height = 720;
    win_cfg.title  = title;
    // --novsync: 素のフレーム時間計測用 (IMMEDIATE)。 60Hz FIFO だと
    // ハイブリッド経路 (~real 7-8ms) が 17ms に張り付いて見える。
    // 既定は FIFO (この環境では IMMEDIATE の windowed present が
    // DWM に反映されず画面が白くなるため、 表示用途では使わない)
    for (int a = 1; a < argc; ++a) {
        if (std::strcmp(argv[a], "--novsync") == 0) win_cfg.vsync = false;
    }
    if (!surface.create(win_cfg)) {
        std::fprintf(stderr, "[init] FATAL: window creation failed\n");
        return 1;
    }
    VulkanContext vk;
    VulkanContextConfig vk_cfg;
    vk_cfg.app_name = "pictor_sharc_demo";
    // PICTOR_SHARC_VALIDATE=1 で Khronos validation layer を有効化
    // (Gate 2 完了条件: 同期警告ゼロの機械確認用)
    vk_cfg.validation = std::getenv("PICTOR_SHARC_VALIDATE") != nullptr;
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
        // 街灯 28 + 提灯/電飾玉 (クラスタ数はシーン依存) を収める
        cfg.max_lights = 256;
    }
    SharcGpuExecutor sharc;
    if (!sharc.initialize(vk, "shaders", cfg)) {
        std::fprintf(stderr, "[init] FATAL: SHaRC executor init failed\n");
        vk.shutdown();
        surface.destroy();
        return 1;
    }

    // ── GPU シーン転写 (メッシュシーン時): 全インスタンスを 1 つの
    //    フラット配列へ結合 → BVH → device-local SSBO。 以後の一次交差は
    //    Pass 0 (sharc_hit.comp) が GPU で行い、 CPU はカメラ変更時の
    //    レイ方向生成だけになる ──
    bool gpu_scene = false;
    uint32_t gpu_tri_count = 0;
    if (mesh_scene.active) {
        sharc_demo::PlyMesh merged;
        std::vector<SharcMaterialGpu> gpu_mats;
        std::vector<sharc_demo::PlyMaterial> atlas_mats;   // texture パス保持
        std::vector<uint32_t> tri_mats;
        for (const auto& inst : mesh_scene.instances) {
            const auto vbase =
                static_cast<uint32_t>(merged.positions.size());
            merged.positions.insert(merged.positions.end(),
                                    inst->mesh.positions.begin(),
                                    inst->mesh.positions.end());
            const auto mat_base = static_cast<uint32_t>(gpu_mats.size());
            const bool override_mat =
                inst->material_override || inst->mesh.materials.empty();
            if (override_mat) {
                SharcMaterialGpu m{};
                m.albedo[0] = inst->ov_albedo.x;
                m.albedo[1] = inst->ov_albedo.y;
                m.albedo[2] = inst->ov_albedo.z;
                m.roughness = inst->ov_roughness;
                m.mfp       = inst->ov_mfp;
                gpu_mats.push_back(m);
                atlas_mats.emplace_back();   // テクスチャなし
            } else {
                for (const auto& pm : inst->mesh.materials) {
                    SharcMaterialGpu m{};
                    m.albedo[0] = pm.albedo[0];
                    m.albedo[1] = pm.albedo[1];
                    m.albedo[2] = pm.albedo[2];
                    m.roughness = pm.roughness;
                    m.mfp       = pm.mfp;
                    m.emissive  = pm.emissive;
                    gpu_mats.push_back(m);
                    atlas_mats.push_back(pm);
                }
            }
            const bool has_uv = !inst->mesh.tri_corner_uvs.empty();
            for (size_t t = 0; t < inst->mesh.triangles.size(); ++t) {
                const auto& tri = inst->mesh.triangles[t];
                merged.triangles.push_back({tri[0] + vbase, tri[1] + vbase,
                                            tri[2] + vbase});
                tri_mats.push_back(override_mat
                                       ? mat_base
                                       : mat_base +
                                             inst->mesh.tri_material[t]);
                if (has_uv) {
                    merged.tri_corner_uvs.push_back(
                        inst->mesh.tri_corner_uvs[t]);
                } else {
                    merged.tri_corner_uvs.push_back(
                        {{{0.0f, 0.0f}, {0.0f, 0.0f}, {0.0f, 0.0f}}});
                }
            }
        }

        // ── アルベドテクスチャ配列 (512^2 × ≤192 層) + マテリアルへの
        //    レイヤ割当。 UV は REPEAT でタイルするため配列方式 ──
        // 1024^2: 看板文字等のディテール確保 (115 層 ≈ 460MB device-local)
        const auto atlas = sharc_demo::build_texture_atlas(atlas_mats, 1024,
                                                           192);
        for (size_t i = 0; i < gpu_mats.size(); ++i) {
            const auto& tex = atlas_mats[i].texture;
            const auto it = tex.empty() ? atlas.layer_of.end()
                                        : atlas.layer_of.find(tex);
            gpu_mats[i].atlas_layer_plus1 =
                (it != atlas.layer_of.end())
                    ? static_cast<float>(it->second + 1)
                    : 0.0f;
        }
        sharc_demo::finalize_mesh(merged);
        sharc_demo::MeshBvh gpu_bvh;
        gpu_bvh.build(merged);

        // ノード / 三角形 (葉順) / マテリアルを GPU レイアウトへ転写
        const auto& nodes = gpu_bvh.nodes();
        const auto& order = gpu_bvh.tri_order();
        std::vector<SharcBvhNodeGpu> gpu_nodes(nodes.size());
        for (size_t i = 0; i < nodes.size(); ++i) {
            std::memcpy(gpu_nodes[i].bmin, nodes[i].bmin, sizeof(float) * 3);
            std::memcpy(gpu_nodes[i].bmax, nodes[i].bmax, sizeof(float) * 3);
            gpu_nodes[i].left  = nodes[i].left;
            gpu_nodes[i].count = nodes[i].count;
        }
        std::vector<SharcTriGpu> gpu_tris(order.size());
        std::vector<uint32_t> gpu_tri_mats(order.size());
        for (size_t i = 0; i < order.size(); ++i) {
            const auto& tri = merged.triangles[order[i]];
            auto& g = gpu_tris[i];
            for (int c = 0; c < 3; ++c) {
                g.v0[c] = merged.positions[tri[0]][c];
                g.v1[c] = merged.positions[tri[1]][c];
                g.v2[c] = merged.positions[tri[2]][c];
            }
            const auto& n0 = merged.normals[tri[0]];
            const auto& n1 = merged.normals[tri[1]];
            const auto& n2 = merged.normals[tri[2]];
            g.n0 = sharc_oct32_encode(n0[0], n0[1], n0[2]);
            g.n1 = sharc_oct32_encode(n1[0], n1[1], n1[2]);
            g.n2 = sharc_oct32_encode(n2[0], n2[1], n2[2]);
            const auto& uv = merged.tri_corner_uvs[order[i]];
            g.uv0[0] = uv[0][0]; g.uv0[1] = uv[0][1];
            g.uv1[0] = uv[1][0]; g.uv1[1] = uv[1][1];
            g.uv2[0] = uv[2][0]; g.uv2[1] = uv[2][1];
            g.pad0 = 0;
            g.pad1 = 0;
            gpu_tri_mats[i] = tri_mats[order[i]];
        }
        SharcSceneUpload up;
        up.nodes          = gpu_nodes.data();
        up.node_count     = static_cast<uint32_t>(gpu_nodes.size());
        up.tris           = gpu_tris.data();
        up.tri_count      = static_cast<uint32_t>(gpu_tris.size());
        up.tri_materials  = gpu_tri_mats.data();
        up.materials      = gpu_mats.data();
        up.material_count = static_cast<uint32_t>(gpu_mats.size());
        if (!atlas.empty()) {
            up.atlas_pixels = atlas.pixels.data();
            up.atlas_size   = atlas.size;
            up.atlas_layers = atlas.layers;
        }
        gpu_scene = sharc.upload_scene(up);
        if (gpu_scene) {
            gpu_tri_count = up.tri_count;
            sharc.set_scene_floor(mesh_scene.use_floor, kFloorY);
            sharc.set_scene_far(g_ray_tmax);
            // 前回終了時のキャッシュがあれば温間スタート (収束待ちゼロ)。
            // 構成 / シーンが変わっていればヘッダ検証で冷間へフォールバック
            if (!cache_path.empty()) sharc.load_cache(cache_path);
        } else {
            std::fprintf(stderr,
                         "[init] WARNING: GPU scene upload failed — "
                         "falling back to CPU trace\n");
        }
    }

    // ── ポストプロセス: Bloom (extract → down ×4 → up ×4、 compute) ──
    sharc_demo::BloomPipeline bloom;
    if (!bloom.initialize(vk, "shaders", kRenderW, kRenderH,
                          sharc.output_buffer(), sharc.output_size())) {
        std::fprintf(stderr, "[init] FATAL: BloomPipeline init failed\n");
        sharc.shutdown();
        vk.shutdown();
        surface.destroy();
        return 1;
    }

    // ── 表示: resolve 出力 SSBO を fragment が直読み + bloom 合成 ──
    sharc_demo::PresentRenderer present;
    if (!present.initialize(vk, "shaders", sharc.output_buffer(),
                            sharc.output_size(), bloom.result_view(),
                            bloom.result_sampler())) {
        std::fprintf(stderr, "[init] FATAL: PresentRenderer init failed\n");
        bloom.shutdown();
        sharc.shutdown();
        vk.shutdown();
        surface.destroy();
        return 1;
    }

    // ── ハイブリッド経路 (ラスタ G-buffer + 太陽シャドウマップ)。
    //    フルレイ経路は Y キーでいつでも A/B 比較できるよう残置 ──
    sharc_demo::GBufferRenderer gbr;
    if (gpu_scene) {
        if (gbr.initialize(vk, "shaders", kRenderW, kRenderH, gpu_tri_count,
                           sharc.scene_tris_buffer(), sharc.scene_tris_size(),
                           sharc.scene_tri_mats_buffer(),
                           sharc.scene_tri_mats_size(),
                           sharc.tri_ao_buffer(), sharc.tri_ao_size(),
                           sharc.scene_materials_buffer(),
                           sharc.scene_materials_size(),
                           sharc.atlas_view(), sharc.atlas_sampler())) {
            sharc.set_gbuffer(gbr.albedo_ao_view(), gbr.normal_rough_view(),
                              gbr.dist_mfp_view(), gbr.sun_shadow_view());
        } else {
            std::fprintf(stderr,
                         "[init] WARNING: G-buffer init failed — full-ray "
                         "path only\n");
        }
    }

    GLFWwindow* win = surface.glfw_window();
    glfwSetMouseButtonCallback(win, mouse_button_cb);
    glfwSetCursorPosCallback(win, cursor_pos_cb);
    glfwSetScrollCallback(win, scroll_cb);
    glfwSetKeyCallback(win, key_cb);

    // D2 はメッシュのみ (球ラダーは D1 専用)
    const auto scene = mesh_scene.active ? std::vector<Sphere>{}
                                         : build_scene();

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

        // ── カメラ (orbit) + ダーティ判定 ──
        //    一次ヒットはカメラとジオメトリだけで決まる (ライトアニメは無関係)。
        //    カメラ静止中は 92 万レイ × BVH の CPU 再トレースを丸ごと省き、
        //    フレームを GPU 4 パス + 読み戻しのみにする (最大の軽量化)。
        const Vec3 target = g_target;
        static float prev_cam[7] = {1e30f, 0, 0, 0, 0, 0, 0};
        const float cam_now[7] = {g_orbit.yaw, g_orbit.pitch, g_orbit.dist,
                                  target.x, target.y, target.z, g_fov_deg};
        bool scene_dirty = false;
        for (int a = 0; a < 7; ++a) {
            if (cam_now[a] != prev_cam[a]) { scene_dirty = true; break; }
        }
        std::memcpy(prev_cam, cam_now, sizeof(cam_now));
        const Vec3 eye{
            target.x + g_orbit.dist * std::cos(g_orbit.pitch) * std::sin(g_orbit.yaw),
            target.y + g_orbit.dist * std::sin(g_orbit.pitch),
            target.z + g_orbit.dist * std::cos(g_orbit.pitch) * std::cos(g_orbit.yaw)};
        const Vec3 fwd   = norm(target - eye);
        const Vec3 right = norm(cross(fwd, Vec3{0, 1, 0}));
        const Vec3 up    = cross(right, fwd);
        const float fov_scale = std::tan(0.5f * g_fov_deg * 3.14159265f / 180.0f);
        const float aspect = static_cast<float>(kRenderW) /
                             static_cast<float>(kRenderH);

        sharc.begin_frame(float3{eye.x, eye.y, eye.z});
        auto* lights = sharc.lights_mapped();
        uint32_t n_lights = 2;
        if (obj_scene) {
            // ── Bistro 日中: 太陽 (Directional) + OBJ から自動抽出した
            //    街灯 (StreetLight_Glass) / 提灯・電飾玉 (Lantern /
            //    StringLights) の全 PointLight + 空光。
            //    posRadius.w = 影響半径 (m): 街灯 7.5 (直径 15m)、
            //    提灯 1.0 (直径 2m)。 全体光量はライト増に合わせ減 ──
            uint32_t li = 0;
            // 太陽: posRadius.w < 0 = directional、 xyz = to-sun 方向。
            // 夕方の終わりかけ: オレンジやや弱め、 全体は日中より一段暗く
            // して街灯/提灯の効果を見せる (neco 指示)
            lights[li++] = SharcLightGpu{
                {kSunDir.x, kSunDir.y, kSunDir.z, -1.0f},
                {1.0f, 0.52f, 0.28f, 1.6f}};
            // 空光 (夕暮れの青紫 directional、 天頂から)。 日陰の視認性は
            // 空光→キャッシュ GI の間接が担う
            lights[li++] = SharcLightGpu{
                {0.0f, 1.0f, 0.0f, -1.0f},
                {0.40f, 0.42f, 0.62f, 0.9f}};
            const uint32_t max_li = 256;
            for (const auto& e : g_emissives) {
                if (li >= max_li) break;
                if (e.kind == 0) {
                    // 街灯: 暖色。 窓関数が中距離 (灯下の地面 ~4.5m) を
                    // 削るため、 半径 9m + 強度 18 で路面プールを出す
                    // (neco 指示: 地面の陰に影響が見えること)
                    lights[li++] = SharcLightGpu{
                        {e.center[0], e.center[1], e.center[2], 9.0f},
                        {1.0f, 0.78f, 0.5f, 18.0f}};
                } else {
                    // 提灯 / 電飾玉: 濃い暖色。 吊り高さ ~3m から地面に
                    // 届くよう半径 4m へ拡大 (直径 2m だと路面に不達)
                    lights[li++] = SharcLightGpu{
                        {e.center[0], e.center[1], e.center[2], 4.0f},
                        {1.0f, 0.62f, 0.38f, 4.0f}};
                }
            }
            if (g_has_prop && li < max_li) {
                // 人物 head の背後光 (肌の透過を出す)
                const Vec3 back = g_prop_pos + g_prop_back * 1.2f;
                lights[li++] = SharcLightGpu{
                    {back.x, back.y + 0.4f, back.z, 3.0f},
                    {1.0f, 0.8f, 0.6f, 30.0f}};
            }
            n_lights = li;
        } else if (mesh_scene.active) {
            // ── D2: 背面光源 (逆光)。 カメラの反対側を横断し、 モデル越しの
            //    透過 (SSS) を見る。 画面外に出る動線も含む ──
            const Vec3 behind = norm(target - eye);
            const float lx = std::sin(light_time * 0.4f) * 2.5f;
            // posRadius.w = 影響半径 (m) — シーン全域をカバーする値
            lights[0] = SharcLightGpu{
                {target.x + behind.x * 4.0f + lx, 1.8f,
                 target.z + behind.z * 4.0f, 20.0f},
                {1.0f, 0.75f, 0.5f, 90.0f}};                       // 逆光・暖色
            lights[1] = SharcLightGpu{{eye.x, eye.y + 2.0f, eye.z, 30.0f},
                                      {0.3f, 0.35f, 0.5f, 15.0f}}; // 弱い前面フィル
        } else {
            // ── D1: 球列の直上を横断する近接光源 (ハイライト肥大検証) ──
            const float lx = std::sin(light_time * 0.6f) * 5.5f;
            lights[0] = SharcLightGpu{{lx, 2.6f, 1.2f, 20.0f},
                                      {1.0f, 0.85f, 0.6f, 60.0f}};  // 近接・暖色
            lights[1] = SharcLightGpu{{0.0f, 8.0f, 6.0f, 30.0f},
                                      {0.4f, 0.5f, 0.8f, 120.0f}};  // フィル・寒色
        }

        // ── CPU 一次レイ: 交差 → ray + shade request 直書き ──
        //    720p (92 万レイ) の BVH 交差は行分割でマルチスレッド化する
        //    (シーンは読み取り専用、 書き込み先は行ごとに独立)。
        auto* rays  = sharc.rays_mapped();
        auto* shade = sharc.shade_requests_mapped();
        auto trace_rows = [&](int y_begin, int y_end) {
            for (int y = y_begin; y < y_end; ++y) {
                for (int x = 0; x < kRenderW; ++x) {
                    const int idx = y * kRenderW + x;
                    const float u = (2.0f * (x + 0.5f) / kRenderW - 1.0f)
                                  * fov_scale * aspect;
                    const float v = (1.0f - 2.0f * (y + 0.5f) / kRenderH)
                                  * fov_scale;
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
        };
        // D1 (プロシージャル、 GPU シーンなし) のみ CPU で一次交差を埋める。
        // メッシュシーンは Pass 0 が GPU 上でレイ生成 + 交差する。
        if (!gpu_scene && scene_dirty) {
            const int n_threads = static_cast<int>(
                std::max(1u, std::thread::hardware_concurrency()));
            const int rows_per = (kRenderH + n_threads - 1) / n_threads;
            std::vector<std::thread> workers;
            workers.reserve(static_cast<size_t>(n_threads));
            for (int t = 0; t < n_threads; ++t) {
                const int y0 = t * rows_per;
                const int y1 = std::min(kRenderH, y0 + rows_per);
                if (y0 >= y1) break;
                workers.emplace_back(trace_rows, y0, y1);
            }
            for (auto& th : workers) th.join();
            sharc.commit_cpu_geometry();   // staging → 本体は次の record()
        }
        sharc.set_albedo_view(g_albedo_view);
        sharc.set_camera(float3{fwd.x, fwd.y, fwd.z},
                         float3{right.x, right.y, right.z},
                         float3{up.x, up.y, up.z}, fov_scale, aspect,
                         static_cast<uint32_t>(kRenderW));
        sharc.set_counts(kRenderW * kRenderH, n_lights);

        // ── ハイブリッド経路の行列更新 (Y キーで A/B 切替) ──
        const bool hybrid_active = gbr.is_initialized() && g_hybrid;
        sharc.set_hybrid(hybrid_active);
        float view_proj[16], sun_vp[16];
        if (hybrid_active) {
            build_view_proj(eye, fwd, right, up, fov_scale, aspect,
                            view_proj);
            build_sun_ortho(kSunDir, g_scene_center,
                            g_scene_extent * 0.6f + 10.0f, sun_vp);
            sharc.set_sun_matrix(sun_vp);
        }

        // ── 全 GPU フレーム: hit + 4 パス → present (readback なし) ──
        const uint32_t image_idx = vk.acquire_next_image();
        if (image_idx == UINT32_MAX) continue;

        auto cmd = vk.command_buffers()[image_idx];
        vkResetCommandBuffer(cmd, 0);
        VkCommandBufferBeginInfo begin_info{
            VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        vkBeginCommandBuffer(cmd, &begin_info);

        if (hybrid_active) {
            const float campos[3] = {eye.x, eye.y, eye.z};
            gbr.record(cmd, view_proj, sun_vp, campos);
        }
        sharc.record(cmd);

        // resolve の出力 → bloom (compute) の読み取り
        {
            VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
            mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1,
                                 &mb, 0, nullptr, 0, nullptr);
        }
        bloom.record(cmd, kExposure);

        // resolve の出力 → fragment 読み
        VkBufferMemoryBarrier out_barrier{
            VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
        out_barrier.srcAccessMask       = VK_ACCESS_SHADER_WRITE_BIT;
        out_barrier.dstAccessMask       = VK_ACCESS_SHADER_READ_BIT;
        out_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        out_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        out_barrier.buffer              = sharc.output_buffer();
        out_barrier.size                = VK_WHOLE_SIZE;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0,
                             nullptr, 1, &out_barrier, 0, nullptr);

        const auto ext = vk.swapchain_extent();
        VkClearValue clear = {{{0.05f, 0.05f, 0.08f, 1.0f}}};
        VkRenderPassBeginInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        rp.renderPass      = vk.default_render_pass();
        rp.framebuffer     = vk.framebuffers()[image_idx];
        rp.renderArea      = {{0, 0}, ext};
        rp.clearValueCount = 1;
        rp.pClearValues    = &clear;
        vkCmdBeginRenderPass(cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);
        present.render(cmd, ext, static_cast<uint32_t>(kRenderW),
                       static_cast<uint32_t>(kRenderH), kExposure,
                       g_albedo_view,
                       g_albedo_view ? 0.0f : kBloomStrength);
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
        VkFence frame_fence = vk.in_flight_fence();
        vkQueueSubmit(vk.graphics_queue(), 1, &present_si, frame_fence);
        vk.present(image_idx);

        // フレーム完了待ち (タイムアウト付き):
        //   1. 次フレームの mapped 書き込み (params / counters) と GPU 読みの
        //      競合を防ぐ (完全同期 — デモは単純さ優先)
        //   2. GPU ハング時に「固まる」のではなく診断を出して落ちる
        const VkResult wait_result = vkWaitForFences(
            vk.device(), 1, &frame_fence, VK_TRUE,
            5ull * 1000ull * 1000ull * 1000ull);   // 5 秒
        if (wait_result == VK_TIMEOUT) {
            std::fprintf(stderr,
                         "[loop] FATAL: frame fence timeout (frame %llu, "
                         "%u requested cells) — GPU hang in SHaRC passes\n",
                         static_cast<unsigned long long>(frame),
                         sharc.request_count());
            break;
        }

        ++frame;
        if (frame % 30 == 0) {
            const float ms = std::chrono::duration<float, std::milli>(
                std::chrono::steady_clock::now() - t_now).count();
            std::printf("[loop] frame %llu: %u requested cells, %.0f ms\n",
                        static_cast<unsigned long long>(frame),
                        sharc.request_count(), ms);
        }
    }

    vk.device_wait_idle();
    // 収束済みキャッシュを直列化 (次回起動の温間スタート用)
    if (gpu_scene && !cache_path.empty()) sharc.save_cache(cache_path);
    present.shutdown();
    bloom.shutdown();
    gbr.shutdown();
    sharc.shutdown();
    vk.shutdown();
    surface.destroy();
    std::printf("[exit] done (%llu frames)\n",
                static_cast<unsigned long long>(frame));
    return 0;
}
