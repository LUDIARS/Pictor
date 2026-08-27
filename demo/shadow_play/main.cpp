/// Pictor Shadow Play Demo — 影絵 (切り絵 + セロファン + 紙の透過)
///
/// 障子紙のスクリーンへ裏から 8 灯 (スポット 4 + ポイント 4、
/// ディレクショナルなし) と月光を当て、樹木と奏者の影絵をレンダリングする。
///
/// 表現:
///   - ハードシャドウ: shadow map 1 サンプル step 比較 (PCF なし) で
///     切り絵のパキッとした輪郭を出す。
///   - セロファン: 各灯に彩度の高いゲル色。スポットのコーン縁もハード。
///   - 紙の透過 (SSS 近似): 入射角依存の exp 減衰 + 多重散乱項で、
///     影の中も仄かに光る紙の空気感を出す。
///
///   - 影響半径 (range): 各灯の減衰へ (1-(d/R)^4)^2 の窓を掛け、
///     重なりの中心は保ったままプール外周の色の洗いを絞る。
///   - 切り絵シート: 月とスクリーンの間 (8 灯より奥) に置いた黒紙レイヤ。
///     カットアウト (月・縦スリット・葉の透かし) だけが月光を透過し、
///     シートに遮られた月光は shadow map 経由で他の何にも影響しない。
///   - 月光ゴッドレイ: 切り抜きを通った月光を観客側の前方体積で
///     レイマーチし、加算合成の光芒として障子の上へ重ねる。
///
/// 実装ノート:
///   受光面がスクリーン平面 1 枚だけなので、ポイントライトも cube map を
///   使わずスクリーン中心向きの 1 フラスタム (広角 perspective) で全影を
///   正確にカバーできる。8 灯 + 月 × 1 枚 = sampled depth 2D array 9 layer。
///
/// 操作:
///   - マウスドラッグ: 観客席側のオービットカメラ / スクロール: ズーム
///   - B: 舞台裏ビュー (切り絵シート確認)
///
/// Build target: pictor_shadow_play_demo

#include "pictor/pictor.h"
#include "pictor/surface/vulkan_context.h"
#include "pictor/surface/glfw_surface_provider.h"

#include <GLFW/glfw3.h>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <chrono>
#include <vector>
#include <string>

using namespace pictor;

// ============================================================
// Math Helpers
// ============================================================

namespace {

constexpr float kPi = 3.14159265358979f;

void mat4_identity(float* m) {
    memset(m, 0, 16 * sizeof(float));
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

void mat4_look_at(float* out, const float* eye, const float* center, const float* up) {
    float fx = center[0] - eye[0], fy = center[1] - eye[1], fz = center[2] - eye[2];
    float fl = std::sqrt(fx*fx + fy*fy + fz*fz);
    fx /= fl; fy /= fl; fz /= fl;

    float sx = fy*up[2] - fz*up[1], sy = fz*up[0] - fx*up[2], sz = fx*up[1] - fy*up[0];
    float sl = std::sqrt(sx*sx + sy*sy + sz*sz);
    sx /= sl; sy /= sl; sz /= sl;

    float ux = sy*fz - sz*fy, uy = sz*fx - sx*fz, uz = sx*fy - sy*fx;

    mat4_identity(out);
    out[0] = sx;  out[4] = sy;  out[8]  = sz;  out[12] = -(sx*eye[0]+sy*eye[1]+sz*eye[2]);
    out[1] = ux;  out[5] = uy;  out[9]  = uz;  out[13] = -(ux*eye[0]+uy*eye[1]+uz*eye[2]);
    out[2] = -fx; out[6] = -fy; out[10] = -fz; out[14] = (fx*eye[0]+fy*eye[1]+fz*eye[2]);
    out[3] = 0;   out[7] = 0;   out[11] = 0;   out[15] = 1.0f;
}

void mat4_perspective(float* out, float fovy_rad, float aspect, float near_z, float far_z) {
    memset(out, 0, 16 * sizeof(float));
    float f = 1.0f / std::tan(fovy_rad * 0.5f);
    out[0]  = f / aspect;
    out[5]  = -f; // Vulkan Y-flip
    out[10] = far_z / (near_z - far_z);
    out[11] = -1.0f;
    out[14] = (near_z * far_z) / (near_z - far_z);
}

void mat4_multiply(float* out, const float* a, const float* b) {
    float tmp[16];
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            tmp[j * 4 + i] = 0.0f;
            for (int k = 0; k < 4; k++) {
                tmp[j * 4 + i] += a[k * 4 + i] * b[j * 4 + k];
            }
        }
    }
    memcpy(out, tmp, 16 * sizeof(float));
}

void mat4_translate(float* out, float x, float y, float z) {
    mat4_identity(out);
    out[12] = x; out[13] = y; out[14] = z;
}

} // anonymous namespace

// ============================================================
// Vertex / Mesh Generation
// ============================================================

struct SpVertex {
    float pos[3];
    float normal[3];
    float uv[2];
};

/// スクリーン (障子紙): XY 平面の矩形、法線 +Z (観客側)
static void generate_screen_quad(std::vector<SpVertex>& vertices,
                                 std::vector<uint32_t>& indices,
                                 float width, float height) {
    vertices.clear();
    indices.clear();
    float hw = width * 0.5f, hh = height * 0.5f;
    const float corners[4][2] = {{-hw,-hh},{hw,-hh},{hw,hh},{-hw,hh}};
    const float uvs[4][2]     = {{0,1},{1,1},{1,0},{0,0}};
    for (int i = 0; i < 4; ++i) {
        SpVertex v;
        v.pos[0] = corners[i][0]; v.pos[1] = corners[i][1]; v.pos[2] = 0.0f;
        v.normal[0] = 0.0f; v.normal[1] = 0.0f; v.normal[2] = 1.0f;
        v.uv[0] = uvs[i][0]; v.uv[1] = uvs[i][1];
        vertices.push_back(v);
    }
    indices = {0, 1, 2, 0, 2, 3};
}

// ============================================================
// Scene Data (must match sp_screen / sp_lit shader SceneUBO, std140)
// ============================================================

constexpr uint32_t kLightCount = 8;
constexpr uint32_t kShadowRes  = 2048;
constexpr float    kScreenW    = 8.0f;
constexpr float    kScreenH    = 4.5f;

// 月ライトは 8 灯とは別枠 (shadow atlas の最終 layer を使う)
constexpr uint32_t kMoonLayer        = kLightCount;
constexpr uint32_t kShadowLayerCount = kLightCount + 1;

// 切り絵シート (カットアウトレイヤ): 月とスクリーンの間に立てた黒紙。
// 月光はシートの切り抜きだけを透過し、シートが遮った光は shadow map 経由で
// 他の何にも影響しない。8 灯 (z >= -8.5) より奥 (z = -9.5) に置くので、
// 幾何的に月光だけがこのシートと交差する。
constexpr float kSheetW = 9.0f;
constexpr float kSheetH = 6.5f;   // sp_cutout.glsl の SP_SHEET_ASPECT と一致させる
constexpr float kSheetY = 6.5f;   // 月光ビーム (上方からの急角度) の軸高さに合わせる
constexpr float kSheetZ = -9.5f;

struct SpLightData {
    float pos_type[4];      // xyz = 位置, w = 0:spot / 1:point
    float dir_cone[4];      // xyz = 照射方向 (spot), w = cos(outer cone)
    float color_params[4];  // rgb = セロファン色, w = 強度
    float range_params[4];  // x = 影響半径 (0 = 無制限), yzw 予備
    float view_proj[16];    // shadow map 用 light VP
};

// shaders/sp_scene.glsl の SceneUBO と 1:1 (std140 手動ミラー)。
// 全メンバが vec4 / mat4 境界なのでパディング差は生じない。
struct SpSceneUBO {
    float view[16];
    float proj[16];
    float view_proj[16];
    float camera_pos[4];
    float params[4];         // x = time, y = ambient, z = sss_strength, w = paper_sigma
    float moon_pos[4];       // xyz = 月ライト位置, w = 有効フラグ
    float moon_dir[4];       // xyz = 月の照射方向, w = 障子直接透過のスケール
    float moon_color[4];     // rgb = 月光色, w = 強度
    float godray_params[4];  // x = 密度, y = 消散係数, z = 前方体積奥行き, w = 紙透過率
    float cam_right[4];      // xyz = カメラ右, w = tan(fovy/2) * aspect
    float cam_up[4];         // xyz = カメラ上, w = tan(fovy/2)
    float cam_fwd[4];        // xyz = カメラ前方, w 予備
    float moon_view_proj[16];
    SpLightData lights[kLightCount];
};

struct PushObject {
    float model[16];
    float tint[4];
};

struct PushDepth {
    float model[16];
    float light_view_proj[16];
};

/// ライト位置からスクリーン 4 隅を全て含む perspective fov を求める。
/// ポイントライトと月ライトの shadow frustum 算出で共用する。
static float fov_covering_screen(const float pos[3], float margin) {
    float vx = -pos[0], vy = -pos[1], vz = -pos[2];
    float vl = std::sqrt(vx*vx + vy*vy + vz*vz);
    float max_cos = 1.0f;
    const float corners[4][2] = {
        {-kScreenW*0.5f, -kScreenH*0.5f}, {kScreenW*0.5f, -kScreenH*0.5f},
        { kScreenW*0.5f,  kScreenH*0.5f}, {-kScreenW*0.5f, kScreenH*0.5f}};
    for (const auto& c : corners) {
        float cx = c[0] - pos[0], cy = c[1] - pos[1], cz = -pos[2];
        float cl = std::sqrt(cx*cx + cy*cy + cz*cz);
        float cosv = (vx*cx + vy*cy + vz*cz) / (vl * cl);
        if (cosv < max_cos) max_cos = cosv;
    }
    max_cos = std::fmax(-1.0f, std::fmin(1.0f, max_cos));
    float fov_rad = 2.0f * std::acos(max_cos) * margin;
    if (fov_rad > 2.6f) fov_rad = 2.6f; // ~150° 上限
    return fov_rad;
}

/// 固定 8 灯 (ライトは動かさない — 影絵はオブジェクト側が動く)。
/// スポット 4 + ポイント 4。ディレクショナルなし。
/// range = 影響半径: (1-(d/R)^4)^2 の窓でプール外周を絞る (0 = 無制限)。
/// 重なりの中心は保ったまま、遠くまで届く色の洗いだけを限定する。
static void setup_lights(SpLightData lights[kLightCount]) {
    struct Def {
        bool  point;
        float pos[3];
        float aim[3];     // spot: 照準点 / point: shadow frustum の向き先
        float cone_deg;   // spot のみ
        float color[3];
        float intensity;
        float range;      // 影響半径 (0 = 無制限)
    };
    static const Def defs[kLightCount] = {
        // --- スポット (指向性、セロファンの円をくっきり落とす) ---
        {false, {-3.5f,  1.8f, -6.5f}, {-1.2f,  0.4f, 0.0f}, 15.0f, {0.95f, 0.18f, 0.12f}, 1.25f, 10.0f}, // 茜
        {false, { 3.2f,  2.2f, -7.0f}, { 1.4f, -0.3f, 0.0f}, 17.0f, {1.00f, 0.62f, 0.15f}, 1.15f, 10.0f}, // 琥珀
        {false, { 0.0f, -2.6f, -6.0f}, { 0.0f,  0.6f, 0.0f}, 19.0f, {0.10f, 0.75f, 0.65f}, 1.05f, 9.5f},  // 青緑
        {false, {-2.8f, -1.5f, -7.5f}, { 0.8f,  0.2f, 0.0f}, 14.0f, {0.80f, 0.20f, 0.75f}, 1.15f, 10.0f}, // 紅紫
        // --- ポイント (全周囲の色の洗い — range でプールを局所化) ---
        {true,  {-4.5f,  0.5f, -4.5f}, {0, 0, 0}, 0.0f, {0.15f, 0.30f, 0.90f},  0.4f,  6.5f},             // 藍
        {true,  { 4.5f, -0.5f, -5.0f}, {0, 0, 0}, 0.0f, {1.00f, 0.55f, 0.65f},  0.35f, 6.5f},             // 桜
        {true,  { 1.5f,  2.8f, -4.0f}, {0, 0, 0}, 0.0f, {0.45f, 0.85f, 0.25f},  0.32f, 6.0f},             // 若草
        {true,  { 0.0f,  0.0f, -8.5f}, {0, 0, 0}, 0.0f, {1.00f, 0.82f, 0.60f},  0.4f, 11.0f},             // 暖白 (主影)
    };

    const float up[3] = {0.0f, 1.0f, 0.0f};

    for (uint32_t i = 0; i < kLightCount; ++i) {
        const Def& d = defs[i];
        SpLightData& l = lights[i];

        l.pos_type[0] = d.pos[0]; l.pos_type[1] = d.pos[1]; l.pos_type[2] = d.pos[2];
        l.pos_type[3] = d.point ? 1.0f : 0.0f;

        float dir[3] = {d.aim[0] - d.pos[0], d.aim[1] - d.pos[1], d.aim[2] - d.pos[2]};
        float dl = std::sqrt(dir[0]*dir[0] + dir[1]*dir[1] + dir[2]*dir[2]);
        dir[0] /= dl; dir[1] /= dl; dir[2] /= dl;
        l.dir_cone[0] = dir[0]; l.dir_cone[1] = dir[1]; l.dir_cone[2] = dir[2];
        l.dir_cone[3] = d.point ? -1.0f : std::cos(d.cone_deg * kPi / 180.0f);

        l.color_params[0] = d.color[0]; l.color_params[1] = d.color[1];
        l.color_params[2] = d.color[2]; l.color_params[3] = d.intensity;

        l.range_params[0] = d.range;
        l.range_params[1] = 0.0f; l.range_params[2] = 0.0f; l.range_params[3] = 0.0f;

        // Shadow frustum:
        //   spot  — コーンを覆う perspective。
        //   point — 受光面がスクリーン平面のみなので、スクリーン中心向きの
        //           1 フラスタムで全影をカバーできる (cube map 不要)。
        //           fov はライト位置からスクリーン 4 隅を含む角度から算出。
        float target[3];
        float fov_rad;
        if (d.point) {
            target[0] = 0.0f; target[1] = 0.0f; target[2] = 0.0f;
            fov_rad = fov_covering_screen(d.pos, 1.15f);
        } else {
            target[0] = d.aim[0]; target[1] = d.aim[1]; target[2] = d.aim[2];
            fov_rad = 2.0f * d.cone_deg * kPi / 180.0f * 1.2f;
        }

        float light_view[16], light_proj[16];
        mat4_look_at(light_view, d.pos, target, up);
        mat4_perspective(light_proj, fov_rad, 1.0f, 0.5f, 40.0f);
        mat4_multiply(l.view_proj, light_proj, light_view);
    }
}

/// 月ライト + ゴッドレイのパラメータ。
/// 月は 8 灯より遥か奥・上方に置き、切り絵シート越しに急角度で
/// スクリーンへ差し込む。シート (z = kSheetZ) は 8 灯 (z >= -8.5) より
/// 奥にあるので、幾何的に月光だけがシートと交差する = シートが遮った
/// 月光は shadow map 経由で「他の何にも影響しない」。
static void setup_moon(SpSceneUBO& scene) {
    const float pos[3]    = {0.0f, 9.0f, -13.0f};
    const float target[3] = {0.0f, 0.0f, 0.0f}; // fov_covering_screen と同じ軸
    const float up[3]     = {0.0f, 1.0f, 0.0f};

    scene.moon_pos[0] = pos[0]; scene.moon_pos[1] = pos[1];
    scene.moon_pos[2] = pos[2]; scene.moon_pos[3] = 1.0f; // 有効

    float dir[3] = {target[0] - pos[0], target[1] - pos[1], target[2] - pos[2]};
    float dl = std::sqrt(dir[0]*dir[0] + dir[1]*dir[1] + dir[2]*dir[2]);
    scene.moon_dir[0] = dir[0] / dl; scene.moon_dir[1] = dir[1] / dl;
    scene.moon_dir[2] = dir[2] / dl; scene.moon_dir[3] = 0.42f;

    // 蒼白い月光
    scene.moon_color[0] = 0.70f; scene.moon_color[1] = 0.84f;
    scene.moon_color[2] = 1.00f; scene.moon_color[3] = 1.7f;

    // ゴッドレイ: x = 密度, y = 消散係数, z = 前方体積の奥行き, w = 紙透過率
    scene.godray_params[0] = 0.20f;
    scene.godray_params[1] = 0.85f;
    scene.godray_params[2] = 3.0f;
    scene.godray_params[3] = 0.32f;

    const float fov_rad = fov_covering_screen(pos, 1.25f);
    float moon_view[16], moon_proj[16];
    mat4_look_at(moon_view, pos, target, up);
    mat4_perspective(moon_proj, fov_rad, 1.0f, 0.5f, 40.0f);
    mat4_multiply(scene.moon_view_proj, moon_proj, moon_view);
}

// ============================================================
// Shadow Play Renderer (Vulkan)
// ============================================================

#ifdef PICTOR_HAS_VULKAN

class ShadowPlayRenderer {
public:
    bool initialize(VulkanContext& vk_ctx, const char* shader_dir) {
        shutdown();
        vk_ctx_ = &vk_ctx;
        device_ = vk_ctx.device();
        if (!device_ || !vk_ctx.default_render_pass()) {
            shutdown();
            return false;
        }

        if (!create_shadow_resources() ||
            !create_shadow_render_pass() ||
            !create_shadow_framebuffers() ||
            !create_descriptor_layout() ||
            !create_pipelines(shader_dir) ||
            !create_buffers() ||
            !create_descriptor_sets()) {
            shutdown();
            return false;
        }

        return true;
    }

    void shutdown() {
        if (device_) vkDeviceWaitIdle(device_);

        auto destroy_pipe = [this](VkPipeline& p) {
            if (p) vkDestroyPipeline(device_, p, nullptr);
            p = VK_NULL_HANDLE;
        };
        destroy_pipe(depth_cut_pipeline_);
        destroy_pipe(depth_trunk_pipeline_);
        destroy_pipe(depth_figure_pipeline_);
        destroy_pipe(screen_pipeline_);
        destroy_pipe(sheet_pipeline_);
        destroy_pipe(godray_pipeline_);
        if (depth_pipeline_layout_)  vkDestroyPipelineLayout(device_, depth_pipeline_layout_, nullptr);
        if (main_pipeline_layout_)   vkDestroyPipelineLayout(device_, main_pipeline_layout_, nullptr);
        if (desc_pool_)       vkDestroyDescriptorPool(device_, desc_pool_, nullptr);
        if (desc_set_layout_) vkDestroyDescriptorSetLayout(device_, desc_set_layout_, nullptr);
        depth_pipeline_layout_ = VK_NULL_HANDLE;
        main_pipeline_layout_ = VK_NULL_HANDLE;
        desc_pool_ = VK_NULL_HANDLE;
        desc_set_ = VK_NULL_HANDLE;
        desc_set_layout_ = VK_NULL_HANDLE;

        auto destroy_buf = [this](VkBuffer& b, VkDeviceMemory& m) {
            if (b) vkDestroyBuffer(device_, b, nullptr);
            if (m) vkFreeMemory(device_, m, nullptr);
            b = VK_NULL_HANDLE; m = VK_NULL_HANDLE;
        };
        destroy_buf(ubo_buffer_, ubo_memory_);
        for (int i = 0; i < kMeshCount; ++i) {
            destroy_buf(mesh_vb_[i], mesh_vb_mem_[i]);
            destroy_buf(mesh_ib_[i], mesh_ib_mem_[i]);
        }

        for (auto fb : shadow_framebuffers_) {
            if (fb) vkDestroyFramebuffer(device_, fb, nullptr);
        }
        shadow_framebuffers_.clear();
        for (auto view : shadow_layer_views_) {
            if (view) vkDestroyImageView(device_, view, nullptr);
        }
        shadow_layer_views_.clear();
        if (shadow_array_view_) vkDestroyImageView(device_, shadow_array_view_, nullptr);
        if (shadow_sampler_)    vkDestroySampler(device_, shadow_sampler_, nullptr);
        if (shadow_image_)      vkDestroyImage(device_, shadow_image_, nullptr);
        if (shadow_memory_)     vkFreeMemory(device_, shadow_memory_, nullptr);
        if (shadow_render_pass_) vkDestroyRenderPass(device_, shadow_render_pass_, nullptr);
        shadow_array_view_ = VK_NULL_HANDLE;
        shadow_sampler_ = VK_NULL_HANDLE;
        shadow_image_ = VK_NULL_HANDLE;
        shadow_memory_ = VK_NULL_HANDLE;
        shadow_render_pass_ = VK_NULL_HANDLE;
        shadow_format_ = VK_FORMAT_UNDEFINED;

        device_ = VK_NULL_HANDLE;
        vk_ctx_ = nullptr;
    }

    ~ShadowPlayRenderer() { shutdown(); }

    bool update_scene(const SpSceneUBO& ubo) {
        void* mapped = nullptr;
        if (vkMapMemory(device_, ubo_memory_, 0, sizeof(SpSceneUBO), 0, &mapped) != VK_SUCCESS)
            return false;
        memcpy(mapped, &ubo, sizeof(SpSceneUBO));
        vkUnmapMemory(device_, ubo_memory_);
        return true;
    }

    /// 1 フレームぶんのコマンド記録。
    /// shadow pass ×9 (木とチェロ奏者 / 月 layer: +切り絵シート) →
    /// main pass (観客ビュー: 障子スクリーン + 月光ゴッドレイ /
    /// 舞台裏ビュー: 切り絵シート + オブジェクト簡易 lit)。
    void render(VkCommandBuffer cmd, uint32_t image_index, VkExtent2D extent,
                const SpSceneUBO& scene, bool backstage,
                const float trunk_model[16], const float figure_model[16]) {
        // 切り絵シートのモデル行列 (固定配置)
        float sheet_model[16];
        mat4_translate(sheet_model, 0.0f, kSheetY, kSheetZ);

        // ---- Shadow passes (8 灯 + 月 = 9 layer) ----
        for (uint32_t i = 0; i < kShadowLayerCount; ++i) {
            VkClearValue clear{};
            clear.depthStencil = {1.0f, 0};

            VkRenderPassBeginInfo rp_info{};
            rp_info.sType       = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
            rp_info.renderPass  = shadow_render_pass_;
            rp_info.framebuffer = shadow_framebuffers_[i];
            rp_info.renderArea  = {{0, 0}, {kShadowRes, kShadowRes}};
            rp_info.clearValueCount = 1;
            rp_info.pClearValues    = &clear;

            vkCmdBeginRenderPass(cmd, &rp_info, VK_SUBPASS_CONTENTS_INLINE);
            VkViewport vp{0, 0, (float)kShadowRes, (float)kShadowRes, 0.0f, 1.0f};
            VkRect2D sc{{0, 0}, {kShadowRes, kShadowRes}};
            vkCmdSetViewport(cmd, 0, 1, &vp);
            vkCmdSetScissor(cmd, 0, 1, &sc);

            PushDepth push{};
            const float* light_vp = (i == kMoonLayer)
                                        ? scene.moon_view_proj
                                        : scene.lights[i].view_proj;
            memcpy(push.light_view_proj, light_vp, sizeof(push.light_view_proj));

            if (i != kMoonLayer) {
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, depth_trunk_pipeline_);
                memcpy(push.model, trunk_model, sizeof(push.model));
                vkCmdPushConstants(cmd, depth_pipeline_layout_, VK_SHADER_STAGE_VERTEX_BIT,
                                   0, sizeof(PushDepth), &push);
                draw_mesh(cmd, kMeshTrunk);
            }
            // 月光は演出上、幹の手前を流れる (参考画像のレイヤリング)。

            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, depth_figure_pipeline_);
            memcpy(push.model, figure_model, sizeof(push.model));
            vkCmdPushConstants(cmd, depth_pipeline_layout_, VK_SHADER_STAGE_VERTEX_BIT,
                               0, sizeof(PushDepth), &push);
            draw_mesh(cmd, kMeshFigure);

            // 月 layer のみ: 切り絵シートをカットアウト付き depth へ焼く。
            // シートは 8 灯より奥にあるため他 layer へ描く必要がない。
            if (i == kMoonLayer) {
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                  depth_cut_pipeline_);
                memcpy(push.model, sheet_model, sizeof(push.model));
                vkCmdPushConstants(cmd, depth_pipeline_layout_, VK_SHADER_STAGE_VERTEX_BIT,
                                   0, sizeof(PushDepth), &push);
                draw_mesh(cmd, kMeshSheet);
            }

            vkCmdEndRenderPass(cmd);
        }

        // ---- Main pass ----
        VkClearValue clear{};
        clear.color = {{0.015f, 0.012f, 0.020f, 1.0f}}; // 舞台の暗がり

        VkRenderPassBeginInfo rp_info{};
        rp_info.sType       = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rp_info.renderPass  = vk_ctx_->default_render_pass();
        rp_info.framebuffer = vk_ctx_->framebuffers()[image_index];
        rp_info.renderArea  = {{0, 0}, extent};
        rp_info.clearValueCount = 1;
        rp_info.pClearValues    = &clear;

        vkCmdBeginRenderPass(cmd, &rp_info, VK_SUBPASS_CONTENTS_INLINE);

        VkViewport viewport{0, 0, (float)extent.width, (float)extent.height, 0.0f, 1.0f};
        VkRect2D scissor{{0, 0}, extent};
        vkCmdSetViewport(cmd, 0, 1, &viewport);
        vkCmdSetScissor(cmd, 0, 1, &scissor);

        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                main_pipeline_layout_, 0, 1, &desc_set_, 0, nullptr);

        if (backstage) {
            // 舞台裏ビュー: 切り絵シート → オブジェクトの順で描く。
            // main render pass には depth attachment が無いので、最奥の
            // シートを先に描く painter's order で前後関係を作る。
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, sheet_pipeline_);
            {
                PushObject push{};
                memcpy(push.model, sheet_model, sizeof(push.model));
                push.tint[0] = 1.0f; push.tint[1] = 1.0f;
                push.tint[2] = 1.0f; push.tint[3] = 1.0f;
                vkCmdPushConstants(cmd, main_pipeline_layout_,
                                   VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                                   0, sizeof(PushObject), &push);
                draw_mesh(cmd, kMeshSheet);
            }

            // 型は depth-only のアルファテストで影を作る。舞台裏では切り絵
            // シートの確認を優先し、型の簡易 lit 表示は行わない。
        } else {
            // 観客ビュー: 障子スクリーン 1 枚に全てが映る
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, screen_pipeline_);

            PushObject push{};
            mat4_identity(push.model);
            push.tint[0] = 1.0f; push.tint[1] = 1.0f; push.tint[2] = 1.0f; push.tint[3] = 1.0f;
            vkCmdPushConstants(cmd, main_pipeline_layout_,
                               VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                               0, sizeof(PushObject), &push);
            draw_mesh(cmd, kMeshScreen);

            // 月光ゴッドレイ: スクリーンの上へ加算合成のフルスクリーン
            // レイマーチを重ねる (頂点バッファなしの 1 三角形)
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, godray_pipeline_);
            vkCmdDraw(cmd, 3, 1, 0, 0);
        }

        vkCmdEndRenderPass(cmd);
    }

private:
    enum MeshIndex {
        kMeshTrunk = 0, kMeshFigure = 1, kMeshScreen = 2, kMeshSheet = 3,
        kMeshCount = 4,
    };

    void draw_mesh(VkCommandBuffer cmd, int mesh) {
        VkDeviceSize offset = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &mesh_vb_[mesh], &offset);
        vkCmdBindIndexBuffer(cmd, mesh_ib_[mesh], 0, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(cmd, mesh_index_count_[mesh], 1, 0, 0, 0);
    }

    // ---- Vulkan helpers ----

    bool create_buffer(VkDeviceSize size, VkBufferUsageFlags usage,
                       VkMemoryPropertyFlags props, VkBuffer& buf, VkDeviceMemory& mem) {
        VkBufferCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        info.size  = size;
        info.usage = usage;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        if (vkCreateBuffer(device_, &info, nullptr, &buf) != VK_SUCCESS) return false;

        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(device_, buf, &req);

        VkMemoryAllocateInfo alloc_info{};
        alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        alloc_info.allocationSize  = req.size;
        alloc_info.memoryTypeIndex = find_memory_type(req.memoryTypeBits, props);
        if (alloc_info.memoryTypeIndex == UINT32_MAX) {
            vkDestroyBuffer(device_, buf, nullptr);
            buf = VK_NULL_HANDLE;
            return false;
        }

        if (vkAllocateMemory(device_, &alloc_info, nullptr, &mem) != VK_SUCCESS) {
            vkDestroyBuffer(device_, buf, nullptr);
            buf = VK_NULL_HANDLE;
            return false;
        }
        if (vkBindBufferMemory(device_, buf, mem, 0) != VK_SUCCESS) {
            vkFreeMemory(device_, mem, nullptr);
            vkDestroyBuffer(device_, buf, nullptr);
            mem = VK_NULL_HANDLE;
            buf = VK_NULL_HANDLE;
            return false;
        }
        return true;
    }

    uint32_t find_memory_type(uint32_t type_filter, VkMemoryPropertyFlags props) {
        VkPhysicalDeviceMemoryProperties mem_props;
        vkGetPhysicalDeviceMemoryProperties(vk_ctx_->physical_device(), &mem_props);
        for (uint32_t i = 0; i < mem_props.memoryTypeCount; i++) {
            if ((type_filter & (1u << i)) &&
                (mem_props.memoryTypes[i].propertyFlags & props) == props) {
                return i;
            }
        }
        return UINT32_MAX;
    }

    bool upload_buffer(VkDeviceMemory mem, const void* data, VkDeviceSize size) {
        void* mapped = nullptr;
        if (vkMapMemory(device_, mem, 0, size, 0, &mapped) != VK_SUCCESS)
            return false;
        memcpy(mapped, data, size);
        vkUnmapMemory(device_, mem);
        return true;
    }

    VkShaderModule load_shader(const char* path) {
        FILE* f = fopen(path, "rb");
        if (!f) return VK_NULL_HANDLE;
        if (fseek(f, 0, SEEK_END) != 0) {
            fclose(f);
            return VK_NULL_HANDLE;
        }
        const long len = ftell(f);
        constexpr long kMaxShaderBytes = 16L * 1024L * 1024L;
        if (len <= 0 || len > kMaxShaderBytes || (len % 4) != 0 ||
            fseek(f, 0, SEEK_SET) != 0) {
            fclose(f);
            return VK_NULL_HANDLE;
        }
        std::vector<uint32_t> code(static_cast<size_t>(len) / sizeof(uint32_t));
        const size_t bytes_read = fread(code.data(), 1, static_cast<size_t>(len), f);
        fclose(f);
        if (bytes_read != static_cast<size_t>(len)) return VK_NULL_HANDLE;

        VkShaderModuleCreateInfo info{};
        info.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        info.codeSize = static_cast<size_t>(len);
        info.pCode    = code.data();

        VkShaderModule mod = VK_NULL_HANDLE;
        if (vkCreateShaderModule(device_, &info, nullptr, &mod) != VK_SUCCESS)
            return VK_NULL_HANDLE;
        return mod;
    }

    // ---- Shadow map resources (sampled depth × 9 layer = 8 灯 + 月) ----

    VkFormat find_shadow_format() const {
        constexpr VkFormatFeatureFlags required =
            VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT |
            VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
        const VkFormat candidates[] = {
            VK_FORMAT_D32_SFLOAT,
            VK_FORMAT_D16_UNORM,
            VK_FORMAT_D32_SFLOAT_S8_UINT,
            VK_FORMAT_D24_UNORM_S8_UINT,
        };
        for (VkFormat format : candidates) {
            VkFormatProperties props{};
            vkGetPhysicalDeviceFormatProperties(vk_ctx_->physical_device(), format, &props);
            if ((props.optimalTilingFeatures & required) == required) return format;
        }
        return VK_FORMAT_UNDEFINED;
    }

    bool create_shadow_resources() {
        shadow_format_ = find_shadow_format();
        if (shadow_format_ == VK_FORMAT_UNDEFINED) return false;

        VkImageCreateInfo img_info{};
        img_info.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        img_info.imageType     = VK_IMAGE_TYPE_2D;
        img_info.format        = shadow_format_;
        img_info.extent        = {kShadowRes, kShadowRes, 1};
        img_info.mipLevels     = 1;
        img_info.arrayLayers   = kShadowLayerCount;
        img_info.samples       = VK_SAMPLE_COUNT_1_BIT;
        img_info.tiling        = VK_IMAGE_TILING_OPTIMAL;
        img_info.usage         = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                                 VK_IMAGE_USAGE_SAMPLED_BIT;
        img_info.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
        img_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        if (vkCreateImage(device_, &img_info, nullptr, &shadow_image_) != VK_SUCCESS)
            return false;

        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(device_, shadow_image_, &req);

        VkMemoryAllocateInfo alloc{};
        alloc.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        alloc.allocationSize  = req.size;
        alloc.memoryTypeIndex = find_memory_type(req.memoryTypeBits,
                                                  VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (alloc.memoryTypeIndex == UINT32_MAX) return false;
        if (vkAllocateMemory(device_, &alloc, nullptr, &shadow_memory_) != VK_SUCCESS)
            return false;
        if (vkBindImageMemory(device_, shadow_image_, shadow_memory_, 0) != VK_SUCCESS)
            return false;

        // サンプリング用 2D array view
        VkImageViewCreateInfo view_info{};
        view_info.sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view_info.image    = shadow_image_;
        view_info.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        view_info.format   = shadow_format_;
        view_info.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_DEPTH_BIT;
        view_info.subresourceRange.baseMipLevel   = 0;
        view_info.subresourceRange.levelCount     = 1;
        view_info.subresourceRange.baseArrayLayer = 0;
        view_info.subresourceRange.layerCount     = kShadowLayerCount;
        if (vkCreateImageView(device_, &view_info, nullptr, &shadow_array_view_) != VK_SUCCESS)
            return false;

        // framebuffer 用 per-layer view
        shadow_layer_views_.resize(kShadowLayerCount);
        for (uint32_t i = 0; i < kShadowLayerCount; ++i) {
            view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
            view_info.subresourceRange.baseArrayLayer = i;
            view_info.subresourceRange.layerCount     = 1;
            if (vkCreateImageView(device_, &view_info, nullptr, &shadow_layer_views_[i]) != VK_SUCCESS)
                return false;
        }

        // ハードシャドウなので NEAREST。範囲外は「遮蔽なし」= 白 border。
        VkSamplerCreateInfo samp_info{};
        samp_info.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        samp_info.magFilter    = VK_FILTER_NEAREST;
        samp_info.minFilter    = VK_FILTER_NEAREST;
        samp_info.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        samp_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        samp_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        samp_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        samp_info.borderColor  = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
        return vkCreateSampler(device_, &samp_info, nullptr, &shadow_sampler_) == VK_SUCCESS;
    }

    bool create_shadow_render_pass() {
        VkAttachmentDescription depth_att{};
        depth_att.format         = shadow_format_;
        depth_att.samples        = VK_SAMPLE_COUNT_1_BIT;
        depth_att.loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depth_att.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
        depth_att.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        depth_att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depth_att.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
        depth_att.finalLayout    = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;

        VkAttachmentReference depth_ref{};
        depth_ref.attachment = 0;
        depth_ref.layout     = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint       = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.pDepthStencilAttachment = &depth_ref;

        VkSubpassDependency deps[2] = {};
        deps[0].srcSubpass    = VK_SUBPASS_EXTERNAL;
        deps[0].dstSubpass    = 0;
        deps[0].srcStageMask  = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        deps[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        deps[0].dstStageMask  = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                                VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        deps[0].dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        deps[1].srcSubpass    = 0;
        deps[1].dstSubpass    = VK_SUBPASS_EXTERNAL;
        deps[1].srcStageMask  = VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        deps[1].srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        deps[1].dstStageMask  = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        deps[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

        VkRenderPassCreateInfo info{};
        info.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        info.attachmentCount = 1;
        info.pAttachments    = &depth_att;
        info.subpassCount    = 1;
        info.pSubpasses      = &subpass;
        info.dependencyCount = 2;
        info.pDependencies   = deps;

        return vkCreateRenderPass(device_, &info, nullptr, &shadow_render_pass_) == VK_SUCCESS;
    }

    bool create_shadow_framebuffers() {
        shadow_framebuffers_.resize(kShadowLayerCount);
        for (uint32_t i = 0; i < kShadowLayerCount; ++i) {
            VkFramebufferCreateInfo info{};
            info.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
            info.renderPass      = shadow_render_pass_;
            info.attachmentCount = 1;
            info.pAttachments    = &shadow_layer_views_[i];
            info.width           = kShadowRes;
            info.height          = kShadowRes;
            info.layers          = 1;
            if (vkCreateFramebuffer(device_, &info, nullptr, &shadow_framebuffers_[i]) != VK_SUCCESS)
                return false;
        }
        return true;
    }

    // ---- Descriptors ----

    bool create_descriptor_layout() {
        VkDescriptorSetLayoutBinding bindings[2] = {};
        bindings[0].binding         = 0;
        bindings[0].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        bindings[0].descriptorCount = 1;
        bindings[0].stageFlags      = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        bindings[1].binding         = 1;
        bindings[1].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[1].descriptorCount = 1;
        bindings[1].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;

        VkDescriptorSetLayoutCreateInfo layout_info{};
        layout_info.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layout_info.bindingCount = 2;
        layout_info.pBindings    = bindings;

        return vkCreateDescriptorSetLayout(device_, &layout_info, nullptr,
                                           &desc_set_layout_) == VK_SUCCESS;
    }

    // ---- Pipelines ----

    bool create_pipelines(const char* shader_dir) {
        std::string base = std::string(shader_dir) + "/";
        VkShaderModule depth_cut_vert = load_shader((base + "sp_depth_cut.vert.spv").c_str());
        VkShaderModule depth_cut_frag = load_shader((base + "sp_depth_cut.frag.spv").c_str());
        VkShaderModule depth_trunk_frag = load_shader((base + "sp_depth_trunk.frag.spv").c_str());
        VkShaderModule depth_figure_frag = load_shader((base + "sp_depth_figure.frag.spv").c_str());
        VkShaderModule screen_vert    = load_shader((base + "sp_screen.vert.spv").c_str());
        VkShaderModule screen_frag    = load_shader((base + "sp_screen.frag.spv").c_str());
        VkShaderModule sheet_vert     = load_shader((base + "sp_sheet.vert.spv").c_str());
        VkShaderModule sheet_frag     = load_shader((base + "sp_sheet.frag.spv").c_str());
        VkShaderModule godray_vert    = load_shader((base + "sp_godray.vert.spv").c_str());
        VkShaderModule godray_frag    = load_shader((base + "sp_godray.frag.spv").c_str());

        auto cleanup_modules = [&]() {
            auto d = [this](VkShaderModule m) {
                if (m) vkDestroyShaderModule(device_, m, nullptr);
            };
            d(depth_cut_vert); d(depth_cut_frag);
            d(depth_trunk_frag); d(depth_figure_frag);
            d(screen_vert); d(screen_frag);
            d(sheet_vert); d(sheet_frag); d(godray_vert); d(godray_frag);
        };

        if (!depth_cut_vert || !depth_cut_frag ||
            !depth_trunk_frag || !depth_figure_frag ||
            !screen_vert || !screen_frag ||
            !sheet_vert || !sheet_frag || !godray_vert || !godray_frag) {
            fprintf(stderr, "ShadowPlayRenderer: failed to load shaders from %s\n", shader_dir);
            cleanup_modules();
            return false;
        }

        // Pipeline layouts
        {
            VkPushConstantRange range{};
            range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
            range.offset     = 0;
            range.size       = sizeof(PushDepth);

            VkPipelineLayoutCreateInfo info{};
            info.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
            info.pushConstantRangeCount = 1;
            info.pPushConstantRanges    = &range;
            if (vkCreatePipelineLayout(device_, &info, nullptr, &depth_pipeline_layout_) != VK_SUCCESS) {
                cleanup_modules();
                return false;
            }
        }
        {
            VkPushConstantRange range{};
            range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
            range.offset     = 0;
            range.size       = sizeof(PushObject);

            VkPipelineLayoutCreateInfo info{};
            info.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
            info.setLayoutCount         = 1;
            info.pSetLayouts            = &desc_set_layout_;
            info.pushConstantRangeCount = 1;
            info.pPushConstantRanges    = &range;
            if (vkCreatePipelineLayout(device_, &info, nullptr, &main_pipeline_layout_) != VK_SUCCESS) {
                cleanup_modules();
                return false;
            }
        }

        // 共有 state
        VkVertexInputBindingDescription bind_desc{};
        bind_desc.binding   = 0;
        bind_desc.stride    = sizeof(SpVertex);
        bind_desc.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

        VkVertexInputAttributeDescription attr_descs[3] = {};
        attr_descs[0] = {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(SpVertex, pos)};
        attr_descs[1] = {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(SpVertex, normal)};
        attr_descs[2] = {2, 0, VK_FORMAT_R32G32_SFLOAT,    offsetof(SpVertex, uv)};

        VkPipelineVertexInputStateCreateInfo vi{};
        vi.sType                           = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        vi.vertexBindingDescriptionCount   = 1;
        vi.pVertexBindingDescriptions      = &bind_desc;
        vi.vertexAttributeDescriptionCount = 3;
        vi.pVertexAttributeDescriptions    = attr_descs;

        VkPipelineInputAssemblyStateCreateInfo ia{};
        ia.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

        VkPipelineViewportStateCreateInfo vp{};
        vp.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        vp.viewportCount = 1;
        vp.scissorCount  = 1;

        VkPipelineMultisampleStateCreateInfo ms{};
        ms.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        VkPipelineDepthStencilStateCreateInfo ds{};
        ds.sType            = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        ds.depthTestEnable  = VK_TRUE;
        ds.depthWriteEnable = VK_TRUE;
        ds.depthCompareOp   = VK_COMPARE_OP_LESS;

        VkPipelineColorBlendAttachmentState blend_att{};
        blend_att.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                   VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

        VkPipelineColorBlendStateCreateInfo cb{};
        cb.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        cb.attachmentCount = 1;
        cb.pAttachments    = &blend_att;

        VkDynamicState dyn_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dyn{};
        dyn.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dyn.dynamicStateCount = 2;
        dyn.pDynamicStates    = dyn_states;

        auto make_stage = [](VkShaderStageFlagBits stage, VkShaderModule mod) {
            VkPipelineShaderStageCreateInfo info{};
            info.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            info.stage  = stage;
            info.module = mod;
            info.pName  = "main";
            return info;
        };

        VkGraphicsPipelineCreateInfo pipe_info{};
        pipe_info.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        pipe_info.pVertexInputState   = &vi;
        pipe_info.pInputAssemblyState = &ia;
        pipe_info.pViewportState      = &vp;
        pipe_info.pMultisampleState   = &ms;
        pipe_info.pDepthStencilState  = &ds;
        pipe_info.pDynamicState       = &dyn;

        bool ok = true;

        // --- Depth-cut (切り絵シート) pipeline ---
        // depth pipeline との違い: カットアウト discard のため uv を渡す
        // frag を使うことと、月がシートの裏面を見るので両面描画にすること。
        if (ok) {
            VkPipelineShaderStageCreateInfo stages[2] = {
                make_stage(VK_SHADER_STAGE_VERTEX_BIT,   depth_cut_vert),
                make_stage(VK_SHADER_STAGE_FRAGMENT_BIT, depth_cut_frag),
            };

            VkPipelineRasterizationStateCreateInfo rs{};
            rs.sType                   = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
            rs.polygonMode             = VK_POLYGON_MODE_FILL;
            rs.cullMode                = VK_CULL_MODE_NONE;
            rs.frontFace               = VK_FRONT_FACE_COUNTER_CLOCKWISE;
            rs.lineWidth               = 1.0f;
            rs.depthBiasEnable         = VK_TRUE;
            rs.depthBiasConstantFactor = 1.25f;
            rs.depthBiasSlopeFactor    = 1.75f;

            VkPipelineColorBlendStateCreateInfo cb_none{};
            cb_none.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
            cb_none.attachmentCount = 0;

            pipe_info.stageCount          = 2;
            pipe_info.pStages             = stages;
            pipe_info.pRasterizationState = &rs;
            pipe_info.pColorBlendState    = &cb_none;
            pipe_info.layout              = depth_pipeline_layout_;
            pipe_info.renderPass          = shadow_render_pass_;

            ok = vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1,
                                           &pipe_info, nullptr, &depth_cut_pipeline_) == VK_SUCCESS;
        }

        // --- Silhouette depth pipelines ---
        // 木と奏者は同じ UV 対応 quad を使い、fragment 側の SDF だけを
        // 分ける。カットアウトと同じ bias / 両面設定で全 shadow layer に焼く。
        auto create_silhouette_pipeline = [&](VkShaderModule frag, VkPipeline* pipeline) {
            VkPipelineShaderStageCreateInfo stages[2] = {
                make_stage(VK_SHADER_STAGE_VERTEX_BIT, depth_cut_vert),
                make_stage(VK_SHADER_STAGE_FRAGMENT_BIT, frag),
            };
            VkPipelineRasterizationStateCreateInfo rs{};
            rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
            rs.polygonMode = VK_POLYGON_MODE_FILL;
            rs.cullMode = VK_CULL_MODE_NONE;
            rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
            rs.lineWidth = 1.0f;
            rs.depthBiasEnable = VK_TRUE;
            rs.depthBiasConstantFactor = 1.25f;
            rs.depthBiasSlopeFactor = 1.75f;
            VkPipelineColorBlendStateCreateInfo cb_none{};
            cb_none.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
            cb_none.attachmentCount = 0;
            pipe_info.stageCount = 2;
            pipe_info.pStages = stages;
            pipe_info.pRasterizationState = &rs;
            pipe_info.pColorBlendState = &cb_none;
            pipe_info.layout = depth_pipeline_layout_;
            pipe_info.renderPass = shadow_render_pass_;
            return vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1,
                                              &pipe_info, nullptr, pipeline) == VK_SUCCESS;
        };
        if (ok) ok = create_silhouette_pipeline(depth_trunk_frag, &depth_trunk_pipeline_);
        if (ok) ok = create_silhouette_pipeline(depth_figure_frag, &depth_figure_pipeline_);

        // --- Screen (障子) pipeline ---
        if (ok) {
            // The context-owned default render pass has no depth attachment.
            // The screen is one quad, and backstage objects do not overlap.
            ds.depthTestEnable  = VK_FALSE;
            ds.depthWriteEnable = VK_FALSE;

            VkPipelineShaderStageCreateInfo stages[2] = {
                make_stage(VK_SHADER_STAGE_VERTEX_BIT,   screen_vert),
                make_stage(VK_SHADER_STAGE_FRAGMENT_BIT, screen_frag),
            };

            VkPipelineRasterizationStateCreateInfo rs{};
            rs.sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
            rs.polygonMode = VK_POLYGON_MODE_FILL;
            rs.cullMode    = VK_CULL_MODE_NONE;
            rs.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;
            rs.lineWidth   = 1.0f;

            pipe_info.stageCount          = 2;
            pipe_info.pStages             = stages;
            pipe_info.pRasterizationState = &rs;
            pipe_info.pColorBlendState    = &cb;
            pipe_info.layout              = main_pipeline_layout_;
            pipe_info.renderPass          = vk_ctx_->default_render_pass();

            ok = vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1,
                                           &pipe_info, nullptr, &screen_pipeline_) == VK_SUCCESS;
        }

        // --- Sheet (舞台裏の切り絵シート可視化) pipeline ---
        // カットアウトを discard するため両面描画。ds は main pass 用に
        // 無効化済み (このパスに depth attachment は無い)。
        if (ok) {
            VkPipelineShaderStageCreateInfo stages[2] = {
                make_stage(VK_SHADER_STAGE_VERTEX_BIT,   sheet_vert),
                make_stage(VK_SHADER_STAGE_FRAGMENT_BIT, sheet_frag),
            };

            VkPipelineRasterizationStateCreateInfo rs{};
            rs.sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
            rs.polygonMode = VK_POLYGON_MODE_FILL;
            rs.cullMode    = VK_CULL_MODE_NONE;
            rs.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;
            rs.lineWidth   = 1.0f;

            pipe_info.stageCount          = 2;
            pipe_info.pStages             = stages;
            pipe_info.pRasterizationState = &rs;
            pipe_info.pColorBlendState    = &cb;
            pipe_info.layout              = main_pipeline_layout_;
            pipe_info.renderPass          = vk_ctx_->default_render_pass();

            ok = vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1,
                                           &pipe_info, nullptr, &sheet_pipeline_) == VK_SUCCESS;
        }

        // --- Godray (月光レイマーチ) pipeline ---
        // 頂点バッファなしのフルスクリーン三角形を、障子スクリーンの上へ
        // 加算合成 (ONE/ONE) で重ねる。
        if (ok) {
            VkPipelineShaderStageCreateInfo stages[2] = {
                make_stage(VK_SHADER_STAGE_VERTEX_BIT,   godray_vert),
                make_stage(VK_SHADER_STAGE_FRAGMENT_BIT, godray_frag),
            };

            VkPipelineVertexInputStateCreateInfo vi_empty{};
            vi_empty.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

            VkPipelineRasterizationStateCreateInfo rs{};
            rs.sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
            rs.polygonMode = VK_POLYGON_MODE_FILL;
            rs.cullMode    = VK_CULL_MODE_NONE;
            rs.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;
            rs.lineWidth   = 1.0f;

            VkPipelineColorBlendAttachmentState blend_add{};
            blend_add.colorWriteMask      = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                            VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
            blend_add.blendEnable         = VK_TRUE;
            blend_add.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
            blend_add.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
            blend_add.colorBlendOp        = VK_BLEND_OP_ADD;
            blend_add.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            blend_add.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            blend_add.alphaBlendOp        = VK_BLEND_OP_ADD;

            VkPipelineColorBlendStateCreateInfo cb_add{};
            cb_add.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
            cb_add.attachmentCount = 1;
            cb_add.pAttachments    = &blend_add;

            pipe_info.stageCount          = 2;
            pipe_info.pStages             = stages;
            pipe_info.pVertexInputState   = &vi_empty;
            pipe_info.pRasterizationState = &rs;
            pipe_info.pColorBlendState    = &cb_add;
            pipe_info.layout              = main_pipeline_layout_;
            pipe_info.renderPass          = vk_ctx_->default_render_pass();

            ok = vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1,
                                           &pipe_info, nullptr, &godray_pipeline_) == VK_SUCCESS;

            pipe_info.pVertexInputState = &vi; // 後続のために共有 state へ戻す
        }

        cleanup_modules();
        return ok;
    }

    // ---- Buffers ----

    bool create_mesh_buffers(int mesh, const std::vector<SpVertex>& vertices,
                             const std::vector<uint32_t>& indices) {
        VkMemoryPropertyFlags host_vis = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                         VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

        mesh_index_count_[mesh] = static_cast<uint32_t>(indices.size());

        if (!create_buffer(vertices.size() * sizeof(SpVertex),
                           VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, host_vis,
                           mesh_vb_[mesh], mesh_vb_mem_[mesh]))
            return false;
        if (!upload_buffer(mesh_vb_mem_[mesh], vertices.data(),
                           vertices.size() * sizeof(SpVertex)))
            return false;

        if (!create_buffer(indices.size() * sizeof(uint32_t),
                           VK_BUFFER_USAGE_INDEX_BUFFER_BIT, host_vis,
                           mesh_ib_[mesh], mesh_ib_mem_[mesh]))
            return false;
        if (!upload_buffer(mesh_ib_mem_[mesh], indices.data(),
                           indices.size() * sizeof(uint32_t)))
            return false;
        return true;
    }

    bool create_buffers() {
        VkMemoryPropertyFlags host_vis = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                         VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

        if (!create_buffer(sizeof(SpSceneUBO), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                           host_vis, ubo_buffer_, ubo_memory_))
            return false;

        std::vector<SpVertex> verts;
        std::vector<uint32_t> idxs;

        generate_screen_quad(verts, idxs, 5.0f, 4.5f);
        if (!create_mesh_buffers(kMeshTrunk, verts, idxs)) return false;

        generate_screen_quad(verts, idxs, 1.1f, 1.5f);
        if (!create_mesh_buffers(kMeshFigure, verts, idxs)) return false;

        generate_screen_quad(verts, idxs, kScreenW, kScreenH);
        if (!create_mesh_buffers(kMeshScreen, verts, idxs)) return false;

        // 切り絵シート (カットアウトは shader 側の手続き判定)
        generate_screen_quad(verts, idxs, kSheetW, kSheetH);
        if (!create_mesh_buffers(kMeshSheet, verts, idxs)) return false;

        return true;
    }

    bool create_descriptor_sets() {
        VkDescriptorPoolSize pool_sizes[2] = {};
        pool_sizes[0].type            = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        pool_sizes[0].descriptorCount = 1;
        pool_sizes[1].type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        pool_sizes[1].descriptorCount = 1;

        VkDescriptorPoolCreateInfo pool_info{};
        pool_info.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pool_info.maxSets       = 1;
        pool_info.poolSizeCount = 2;
        pool_info.pPoolSizes    = pool_sizes;

        if (vkCreateDescriptorPool(device_, &pool_info, nullptr, &desc_pool_) != VK_SUCCESS)
            return false;

        VkDescriptorSetAllocateInfo alloc_info{};
        alloc_info.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        alloc_info.descriptorPool     = desc_pool_;
        alloc_info.descriptorSetCount = 1;
        alloc_info.pSetLayouts        = &desc_set_layout_;

        if (vkAllocateDescriptorSets(device_, &alloc_info, &desc_set_) != VK_SUCCESS)
            return false;

        VkDescriptorBufferInfo ubo_info{ubo_buffer_, 0, sizeof(SpSceneUBO)};

        VkDescriptorImageInfo img_info{};
        img_info.sampler     = shadow_sampler_;
        img_info.imageView   = shadow_array_view_;
        img_info.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;

        VkWriteDescriptorSet writes[2] = {};
        writes[0].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet          = desc_set_;
        writes[0].dstBinding      = 0;
        writes[0].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[0].descriptorCount = 1;
        writes[0].pBufferInfo     = &ubo_info;
        writes[1].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet          = desc_set_;
        writes[1].dstBinding      = 1;
        writes[1].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[1].descriptorCount = 1;
        writes[1].pImageInfo      = &img_info;

        vkUpdateDescriptorSets(device_, 2, writes, 0, nullptr);
        return true;
    }

    // ---- Members ----

    VulkanContext* vk_ctx_ = nullptr;
    VkDevice device_ = VK_NULL_HANDLE;

    // Shadow map (sampled depth × 9 layer = 8 灯 + 月)
    VkFormat       shadow_format_ = VK_FORMAT_UNDEFINED;
    VkImage        shadow_image_  = VK_NULL_HANDLE;
    VkDeviceMemory shadow_memory_ = VK_NULL_HANDLE;
    VkImageView    shadow_array_view_ = VK_NULL_HANDLE;
    std::vector<VkImageView> shadow_layer_views_;
    VkSampler      shadow_sampler_ = VK_NULL_HANDLE;
    VkRenderPass   shadow_render_pass_ = VK_NULL_HANDLE;
    std::vector<VkFramebuffer> shadow_framebuffers_;

    // Pipelines
    VkPipelineLayout depth_pipeline_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout main_pipeline_layout_  = VK_NULL_HANDLE;
    VkPipeline depth_cut_pipeline_ = VK_NULL_HANDLE; // 切り絵シート (カットアウト discard)
    VkPipeline depth_trunk_pipeline_ = VK_NULL_HANDLE;
    VkPipeline depth_figure_pipeline_ = VK_NULL_HANDLE;
    VkPipeline screen_pipeline_    = VK_NULL_HANDLE;
    VkPipeline sheet_pipeline_     = VK_NULL_HANDLE; // 舞台裏のシート可視化
    VkPipeline godray_pipeline_    = VK_NULL_HANDLE; // 月光ゴッドレイ (加算合成)
    VkDescriptorSetLayout desc_set_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool desc_pool_ = VK_NULL_HANDLE;
    VkDescriptorSet desc_set_ = VK_NULL_HANDLE;

    // Buffers
    VkBuffer ubo_buffer_ = VK_NULL_HANDLE;
    VkDeviceMemory ubo_memory_ = VK_NULL_HANDLE;
    VkBuffer mesh_vb_[kMeshCount] = {};
    VkBuffer mesh_ib_[kMeshCount] = {};
    VkDeviceMemory mesh_vb_mem_[kMeshCount] = {};
    VkDeviceMemory mesh_ib_mem_[kMeshCount] = {};
    uint32_t mesh_index_count_[kMeshCount] = {};
};

enum class FrameResult { Rendered, Skipped, Failed };

static FrameResult render_frame(VulkanContext& vk_ctx,
                                ShadowPlayRenderer& renderer,
                                const SpSceneUBO& scene,
                                bool backstage,
                                const float trunk_model[16],
                                const float figure_model[16]) {
    const uint32_t image_index = vk_ctx.acquire_next_image();
    if (image_index == UINT32_MAX) return FrameResult::Skipped;

    if (!renderer.update_scene(scene)) {
        fprintf(stderr, "Failed to update scene uniform buffer\n");
        return FrameResult::Failed;
    }

    VkCommandBuffer cmd = vk_ctx.command_buffers()[image_index];
    if (vkResetCommandBuffer(cmd, 0) != VK_SUCCESS) {
        fprintf(stderr, "Failed to reset command buffer\n");
        return FrameResult::Failed;
    }

    VkCommandBufferBeginInfo begin_info{};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    if (vkBeginCommandBuffer(cmd, &begin_info) != VK_SUCCESS) {
        fprintf(stderr, "Failed to begin command buffer\n");
        return FrameResult::Failed;
    }

    renderer.render(cmd, image_index, vk_ctx.swapchain_extent(),
                    scene, backstage, trunk_model, figure_model);

    if (vkEndCommandBuffer(cmd) != VK_SUCCESS) {
        fprintf(stderr, "Failed to end command buffer\n");
        return FrameResult::Failed;
    }

    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSemaphore wait_sem = vk_ctx.image_available_semaphore();
    VkSemaphore signal_sem = vk_ctx.render_finished_semaphore();

    VkSubmitInfo submit{};
    submit.sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.waitSemaphoreCount   = 1;
    submit.pWaitSemaphores      = &wait_sem;
    submit.pWaitDstStageMask    = &wait_stage;
    submit.commandBufferCount   = 1;
    submit.pCommandBuffers      = &cmd;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores    = &signal_sem;

    if (vkQueueSubmit(vk_ctx.graphics_queue(), 1, &submit,
                      vk_ctx.in_flight_fence()) != VK_SUCCESS) {
        fprintf(stderr, "Failed to submit frame\n");
        return FrameResult::Failed;
    }
    vk_ctx.present(image_index);
    return FrameResult::Rendered;
}

#endif // PICTOR_HAS_VULKAN

// ============================================================
// Main
// ============================================================

int main() {
    printf("=== Pictor Shadow Play Demo — 影絵 (8 lights, hard shadow, SSS) ===\n\n");

    // ---- 1. GLFW Window ----
    GlfwSurfaceProvider surface_provider;
    GlfwWindowConfig win_cfg;
    win_cfg.width  = 1600;
    win_cfg.height = 900;
    win_cfg.title  = "Pictor — Shadow Play Demo (影絵)";
    win_cfg.vsync  = true;

    if (!surface_provider.create(win_cfg)) {
        fprintf(stderr, "Failed to create GLFW window\n");
        return 1;
    }

    // ---- 2. Vulkan Context ----
    VulkanContext vk_ctx;
    VulkanContextConfig vk_cfg;
    vk_cfg.app_name   = "Pictor Shadow Play Demo";
    vk_cfg.validation = true;
    // This compact demo owns one host-written scene UBO, so serialize frames
    // instead of allowing a later CPU frame to overwrite data still in use.
    vk_cfg.frames_in_flight = 1;

    if (!vk_ctx.initialize(&surface_provider, vk_cfg)) {
        fprintf(stderr, "Failed to initialize Vulkan context\n");
        surface_provider.destroy();
        return 1;
    }

    uint32_t screen_w = 1600, screen_h = 900;
#ifdef PICTOR_HAS_VULKAN
    screen_w = vk_ctx.swapchain_extent().width;
    screen_h = vk_ctx.swapchain_extent().height;
#endif
    printf("Vulkan initialized: %ux%u\n", screen_w, screen_h);

    // ---- 3. Shadow Play Renderer ----
#ifdef PICTOR_HAS_VULKAN
    ShadowPlayRenderer sp_renderer;
    std::string shader_dir = "shaders";
    if (!sp_renderer.initialize(vk_ctx, shader_dir.c_str())) {
        shader_dir = "../shaders";
        if (!sp_renderer.initialize(vk_ctx, shader_dir.c_str())) {
            fprintf(stderr, "Failed to initialize Shadow Play renderer\n");
            vk_ctx.shutdown();
            surface_provider.destroy();
            return 1;
        }
    }
    printf("Shadow Play Renderer initialized.\n");
#endif

    // ---- 4. Scene (固定 8 灯) ----
    SpSceneUBO scene{};
    setup_lights(scene.lights);
    setup_moon(scene);        // 月ライト + 切り絵シート越しのゴッドレイ
    scene.params[1] = 0.008f; // ambient
    scene.params[2] = 0.022f; // sss_strength (多重散乱)
    scene.params[3] = 0.35f;  // paper_sigma (紙の光学的厚み)

    // ---- 5. Orbit Camera (観客席側に制限) ----
    struct OrbitCamera {
        float yaw   = 0.0f;   // 正面 = +Z 側
        float pitch = 0.05f;
        float radius = 7.5f;
        double lastMouseX = 0.0, lastMouseY = 0.0;
        bool dragging = false;
    };
    static OrbitCamera orbit_cam;
    static bool backstage = false;

    GLFWwindow* win = surface_provider.glfw_window();
    glfwSetMouseButtonCallback(win, [](GLFWwindow* w, int button, int action, int /*mods*/) {
        if (button == GLFW_MOUSE_BUTTON_LEFT) {
            orbit_cam.dragging = (action == GLFW_PRESS);
            if (orbit_cam.dragging)
                glfwGetCursorPos(w, &orbit_cam.lastMouseX, &orbit_cam.lastMouseY);
        }
    });
    glfwSetCursorPosCallback(win, [](GLFWwindow*, double xpos, double ypos) {
        if (!orbit_cam.dragging) return;
        double dx = xpos - orbit_cam.lastMouseX;
        double dy = ypos - orbit_cam.lastMouseY;
        orbit_cam.lastMouseX = xpos;
        orbit_cam.lastMouseY = ypos;
        orbit_cam.yaw   -= static_cast<float>(dx) * 0.004f;
        orbit_cam.pitch += static_cast<float>(dy) * 0.004f;
        // 観客席の範囲に制限 (スクリーン正面のみ)
        if (orbit_cam.yaw > 0.85f)    orbit_cam.yaw = 0.85f;
        if (orbit_cam.yaw < -0.85f)   orbit_cam.yaw = -0.85f;
        if (orbit_cam.pitch > 0.7f)   orbit_cam.pitch = 0.7f;
        if (orbit_cam.pitch < -0.7f)  orbit_cam.pitch = -0.7f;
    });
    glfwSetScrollCallback(win, [](GLFWwindow*, double /*xoffset*/, double yoffset) {
        orbit_cam.radius -= static_cast<float>(yoffset) * 0.8f;
        if (orbit_cam.radius < 3.0f)  orbit_cam.radius = 3.0f;
        if (orbit_cam.radius > 20.0f) orbit_cam.radius = 20.0f;
    });

    printf("\nShadow play setup:\n");
    printf("  - Screen (shoji): %.1f x %.1f at z=0\n", kScreenW, kScreenH);
    printf("  - Casters: cutout tree + cello player (static silhouettes)\n");
    printf("  - 8 fixed lights: 4 spot + 4 point (no directional)\n");
    printf("  - Light range windows: per-light influence radius\n");
    printf("  - Moon + kirie sheet: %.1f x %.1f at z=%.1f (cutout layer)\n",
           kSheetW, kSheetH, kSheetZ);
    printf("  - Moon god rays through cutouts (additive ray-march)\n");
    printf("  - Hard shadows: 1-sample step compare, no PCF\n");
    printf("  - Cellophane gels + paper transmission (SSS approx)\n");
    printf("  - Mouse drag: orbit (audience side), Scroll: zoom\n");
    printf("  - Press 'B' to toggle backstage view\n");
    printf("\nEntering main loop. Close the window to exit.\n\n");

    auto start = std::chrono::high_resolution_clock::now();
    uint64_t frame_count = 0;
    bool b_key_was_pressed = false;

    while (!surface_provider.should_close()) {
        surface_provider.poll_events();

        {
            bool b_pressed = glfwGetKey(win, GLFW_KEY_B) == GLFW_PRESS;
            if (b_pressed && !b_key_was_pressed) {
                backstage = !backstage;
                printf("[View] %s\n", backstage ? "backstage" : "audience");
            }
            b_key_was_pressed = b_pressed;
        }

        auto now = std::chrono::high_resolution_clock::now();
        float elapsed = std::chrono::duration<float>(now - start).count();

        // ---- 固定の切り絵型モデル行列 ----
        float trunk_model[16];
        float figure_model[16];
        mat4_translate(trunk_model, 0.0f, -0.05f, -1.05f);
        // 影の shoji 到達域が y [-1.35, -0.35] (水際の上) に収まる高さ。
        // 月 (0,9,-13) からの射影倍率 ~1.06 で見積もる。
        mat4_translate(figure_model, 0.20f, -0.40f, -0.70f);

        // ---- カメラ ----
        float yaw = orbit_cam.yaw + (backstage ? kPi : 0.0f);
        float cos_pitch = std::cos(orbit_cam.pitch);
        float center[3] = {
            0.0f,
            backstage ? kSheetY : 0.0f,
            backstage ? kSheetZ : 0.0f,
        };
        // シート全体が既定の縦 FOV に収まる距離を舞台裏ビューでは確保する。
        float camera_radius = backstage ? std::fmax(orbit_cam.radius, 11.5f)
                                        : orbit_cam.radius;
        float eye[3] = {
            center[0] + camera_radius * cos_pitch * std::sin(yaw),
            center[1] + camera_radius * std::sin(orbit_cam.pitch),
            center[2] + camera_radius * cos_pitch * std::cos(yaw),
        };
        float up[3] = {0.0f, 1.0f, 0.0f};

#ifdef PICTOR_HAS_VULKAN
        screen_w = vk_ctx.swapchain_extent().width;
        screen_h = vk_ctx.swapchain_extent().height;
#endif
        if (screen_h == 0) continue;
        constexpr float kFovY = 0.6f;
        float aspect = static_cast<float>(screen_w) / static_cast<float>(screen_h);
        mat4_look_at(scene.view, eye, center, up);
        mat4_perspective(scene.proj, kFovY, aspect, 0.1f, 100.0f);
        mat4_multiply(scene.view_proj, scene.proj, scene.view);
        scene.camera_pos[0] = eye[0];
        scene.camera_pos[1] = eye[1];
        scene.camera_pos[2] = eye[2];
        scene.params[0] = elapsed;

        // ゴッドレイのレイ再構成用カメラ基底 (view 行列の行 = 右/上/前方)。
        // view の第 3 行は -forward なので符号を戻す。
        const float tan_half = std::tan(kFovY * 0.5f);
        scene.cam_right[0] = scene.view[0];
        scene.cam_right[1] = scene.view[4];
        scene.cam_right[2] = scene.view[8];
        scene.cam_right[3] = tan_half * aspect;
        scene.cam_up[0] = scene.view[1];
        scene.cam_up[1] = scene.view[5];
        scene.cam_up[2] = scene.view[9];
        scene.cam_up[3] = tan_half;
        scene.cam_fwd[0] = -scene.view[2];
        scene.cam_fwd[1] = -scene.view[6];
        scene.cam_fwd[2] = -scene.view[10];
        scene.cam_fwd[3] = 0.0f;

        // ---- 描画 ----
#ifdef PICTOR_HAS_VULKAN
        const FrameResult frame_result = render_frame(
            vk_ctx, sp_renderer, scene, backstage, trunk_model, figure_model);
        if (frame_result == FrameResult::Skipped) continue;
        if (frame_result == FrameResult::Failed) break;
#endif

        frame_count++;
        if (frame_count % 300 == 0) {
            printf("[Frame %lu] t=%.1fs\n", (unsigned long)frame_count, elapsed);
        }
    }

    // ---- 6. Cleanup ----
    vk_ctx.device_wait_idle();

#ifdef PICTOR_HAS_VULKAN
    sp_renderer.shutdown();
#endif

    vk_ctx.shutdown();
    surface_provider.destroy();

    printf("\nShadow play demo finished. Total frames: %lu\n", (unsigned long)frame_count);
    return 0;
}
