#pragma once

/// SharcGpuExecutor — SHaRC 拡張ライティングキャッシュの実 GPU 実行。
///
/// spec: `pictor-sharc-ext-design.md` §3 (4 パスパイプライン)。
/// `GIGpuExecutor` と同じ host-driven 契約で、 SHaRC の 4 compute パス
/// (march / compact / update / resolve) の Vulkan 資源を自己所有する:
///
///   1. init 後、 ホストが毎フレーム `begin_frame()` → mapped ポインタ
///      (`rays_mapped()` / `shade_requests_mapped()` / `lights_mapped()`)
///      へ直書き → `set_counts()`。
///   2. ホストのコマンドバッファへ `record()` で 4 dispatch を記録する。
///      パス間は buffer barrier で compute write → read を保証。
///      update パスは compact が生成した indirect 引数で dispatch する。
///   3. GPU 完了後 (fence / wait idle はホスト責務)、 `output_mapped()` から
///      解決済み放射輝度 (vec4 × ray 数) を読む。
///
/// バッファは全て host-visible + coherent (GIGpuExecutor と同じ方針 —
/// デモ / 検証プロトコル §5 では転送最適化より単純さを取る)。
///
/// SRP: Vulkan 資源管理と dispatch 記録のみ。 セル表現の意味論は
/// `shaders/sharc/*.glsl` と `sharc_types.h`、 シーン生成はデモ側の責務。

#include "pictor/core/types.h"
#include "pictor/gi/sharc_types.h"

#include <cstdint>
#include <string>

#ifdef PICTOR_HAS_VULKAN
#include <vulkan/vulkan.h>
#endif

namespace pictor {

class VulkanContext;

/// 初期化パラメータ (途中変更は shutdown → 再 init)。
struct SharcConfig {
    uint32_t table_size     = 1u << 16;  ///< ハッシュスロット数 (2^n 必須)
    uint32_t max_rays       = 320 * 180; ///< march / resolve の最大レイ数
    uint32_t max_hits       = 1u << 20;  ///< ヒット列容量
    uint32_t max_lights     = 64;        ///< ライトバッファ容量
    float    base_cell_size = 0.25f;     ///< level 0 セル一辺 (m)
    uint32_t level_count    = 8;
    float    ema_alpha      = 0.05f;     ///< 時間平均係数
    float    level_bias     = 0.5f;      ///< 距離→レベル選択バイアス
    float    sss_mfp_scale  = 3.0f;      ///< MFP→レベル選択倍率
    uint32_t max_ray_steps  = 64;        ///< march の最大セルステップ
    uint32_t stale_frames   = 240;       ///< エビクション閾値
    float    hit_epsilon    = 1e-3f;
};

/// GPU 入力レイ (shaders/sharc/sharc_march.comp の SharcRay と同一)。
struct SharcRayGpu {
    float origin_tmin[4];   ///< xyz = 原点, w = tMin
    float dir_tmax[4];      ///< xyz = 方向 (正規化), w = tMax
};

/// GPU シェーディング要求 (sharc_resolve.comp の SharcShadeRequest と同一)。
struct SharcShadeRequestGpu {
    float pos_rough[4];     ///< xyz = ヒット点, w = roughness
    float normal_mfp[4];    ///< xyz = 法線, w = SSS MFP (0 = 無効)
    float albedo_view[4];   ///< rgb = albedo, a = 予約
    float view_dir[4];      ///< xyz = ヒット点→カメラ, w = 予約
};

/// GPU ライト (sharc_update.comp の SharcLight と同一)。
struct SharcLightGpu {
    float pos_radius[4];       ///< xyz = 位置, w = 半径
    float color_intensity[4];  ///< rgb = 色, a = 強度
};

/// GPU シーン (hit パス) の一括アップロード内容。 全てフラット DoD 配列で、
/// デモ側の BVH / メッシュをレイアウト変換なしに近い形で転写する。
struct SharcSceneUpload {
    const SharcBvhNodeGpu* nodes = nullptr;
    uint32_t node_count = 0;
    const SharcTriGpu* tris = nullptr;        ///< 葉順に並べ替え済み
    uint32_t tri_count = 0;
    const uint32_t* tri_materials = nullptr;  ///< [tri_count]
    const SharcMaterialGpu* materials = nullptr;
    uint32_t material_count = 0;
    /// アルベドテクスチャ配列 (RGBA8 sRGB、 [layer][size*size*4] 連結)。
    /// nullptr / 0 レイヤなら 1x1 白ダミーを維持する。
    const uint8_t* atlas_pixels = nullptr;
    uint32_t atlas_size   = 0;
    uint32_t atlas_layers = 0;
};

class SharcGpuExecutor {
public:
    SharcGpuExecutor() = default;
    ~SharcGpuExecutor();

    SharcGpuExecutor(const SharcGpuExecutor&) = delete;
    SharcGpuExecutor& operator=(const SharcGpuExecutor&) = delete;

    bool is_initialized() const { return initialized_; }

#ifdef PICTOR_HAS_VULKAN
    /// 実 Vulkan リソースを確保する。 `shader_dir` はコンパイル済み .spv の
    /// ディレクトリ (sharc_{march,compact,update,resolve}.comp.spv を要求)。
    bool initialize(VulkanContext& vk, const std::string& shader_dir,
                    const SharcConfig& config);

    /// GPU シーン (BVH + 三角形 + マテリアル) を device-local バッファへ
    /// 転送し、 hit パス (GPU 一次交差) を有効化する。 init 後に 1 回。
    bool upload_scene(const SharcSceneUpload& scene);

    /// 解析床 (D2 チェッカー) の有効化。 upload_scene 済みの時のみ意味を持つ。
    void set_scene_floor(bool enabled, float floor_y);

    /// hit パスの初期 tMax (シーンの遠方距離)。
    void set_scene_far(float ray_far);

    /// カメラ基底を UBO へ反映する (hit パスの GPU レイ生成用)。
    /// fov_scale = tan(fov/2)。 render_width はレイ index → 画素の変換用。
    void set_camera(const float3& fwd, const float3& right, const float3& up,
                    float fov_scale, float aspect, uint32_t render_width);

    /// フレーム開始: フレーム番号 / カメラを UBO へ反映し、
    /// per-frame カウンタ (hit / request) をリセットする。
    void begin_frame(const float3& camera_pos);

    /// 今フレームの入力数を確定する (mapped 直書きの後に呼ぶ)。
    void set_counts(uint32_t ray_count, uint32_t light_count);

    /// 4 パスの dispatch を記録する (compute キュー / graphics キュー兼用)。
    void record(VkCommandBuffer cmd);

    // ---- ホスト直書き用 mapped ポインタ (DoD: コピー層を挟まない) ----
    // rays / shade は staging (host-visible)。 GPU 本体は device-local で、
    // commit_cpu_geometry() を呼んだ次の record() が転送する (CPU 一次交差
    // を使う D1 専用 — メッシュシーンは hit パスが GPU 上で生成するので不要)。

    SharcRayGpu*          rays_mapped();
    SharcShadeRequestGpu* shade_requests_mapped();
    SharcLightGpu*        lights_mapped();

    /// staging へ書いた rays / shade を次の record() で GPU 本体へ転送する。
    void commit_cpu_geometry() { cpu_geometry_dirty_ = true; }

    /// 出力バッファの実体 (present 側 fragment が SSBO として直読みする —
    /// readback レスの全 GPU 経路)。
    VkBuffer     output_buffer() const { return output_.buf; }
    VkDeviceSize output_size() const   { return output_.size; }

    /// 今フレームの要求セル数 (GPU 完了後に読める、 HUD 用)。
    uint32_t request_count() const;

    uint32_t frame_index() const { return frame_index_; }
#endif

    void shutdown();

private:
#ifdef PICTOR_HAS_VULKAN
    struct Buffer {
        VkBuffer       buf    = VK_NULL_HANDLE;
        VkDeviceMemory mem    = VK_NULL_HANDLE;
        void*          mapped = nullptr;
        VkDeviceSize   size   = 0;
    };

    struct Pass {
        VkPipeline pipeline = VK_NULL_HANDLE;
    };

    bool create_buffer_(Buffer& out, VkDeviceSize size,
                        VkBufferUsageFlags usage, bool device_local = false);
    /// device-local バッファ + staging 経由の一括転送 (シーン用、 mapped なし)。
    bool create_device_buffer_(Buffer& out, const void* data,
                               VkDeviceSize size);
    void destroy_buffer_(Buffer& b);
    bool create_pass_(Pass& out, const std::string& spv_path);
    void write_params_();
    void write_scene_descriptors_();
    bool create_atlas_(const uint8_t* pixels, uint32_t size, uint32_t layers);
    void destroy_atlas_();
    void barrier_(VkCommandBuffer cmd, VkBuffer buf,
                  VkAccessFlags src, VkAccessFlags dst,
                  VkPipelineStageFlags dst_stage);

    VulkanContext* vk_     = nullptr;
    VkDevice       device_ = VK_NULL_HANDLE;

    SharcConfig config_{};
    uint32_t    ray_count_   = 0;
    uint32_t    light_count_ = 0;
    uint32_t    frame_index_ = 0;
    float3      camera_pos_{};

    Buffer params_;       // SharcParams UBO            (binding 0)
    Buffer keys_;         // ハッシュキー                (binding 1)
    Buffer cells_;        // セル本体 80B×slots          (binding 2)
    Buffer rays_;         // 入力レイ                    (binding 3)
    Buffer hits_;         // ヒット列                    (binding 4)
    Buffer counters_;     // カウンタ                    (binding 5)
    Buffer requests_;     // 要求セルリスト              (binding 6)
    Buffer stamps_;       // 要求スタンプ                (binding 7)
    Buffer indirect_;     // indirect dispatch 引数      (binding 8)
    Buffer lights_;       // ライトリスト                (binding 9)
    Buffer reservoirs_;   // per-cell reservoir          (binding 10)
    Buffer cell_pos_;     // グリッド座標逆引き          (binding 11)
    Buffer shade_;        // シェーディング要求          (binding 12)
    Buffer output_;       // 解決結果                    (binding 13)
    Buffer scene_nodes_;  // BVH ノード (device-local)   (binding 14)
    Buffer scene_tris_;   // 三角形 (device-local)       (binding 15)
    Buffer scene_tri_mats_; // per-tri マテリアル id     (binding 16)
    Buffer scene_materials_; // マテリアル表             (binding 17)
    Buffer rays_staging_;    // D1 CPU 経路の転送元 (host-visible)
    Buffer shade_staging_;   // 同上
    Buffer counters_rb_;     // request_count 読み出し用 (host-visible 16B)

    // アルベドテクスチャ配列 (binding 18、 未使用時は 1x1 白ダミー)
    VkImage        atlas_image_   = VK_NULL_HANDLE;
    VkDeviceMemory atlas_mem_     = VK_NULL_HANDLE;
    VkImageView    atlas_view_    = VK_NULL_HANDLE;
    VkSampler      atlas_sampler_ = VK_NULL_HANDLE;

    VkDescriptorSetLayout dsl_       = VK_NULL_HANDLE;
    VkPipelineLayout      layout_    = VK_NULL_HANDLE;
    VkDescriptorPool      desc_pool_ = VK_NULL_HANDLE;
    VkDescriptorSet       desc_set_  = VK_NULL_HANDLE;

    Pass hit_;
    Pass march_;
    Pass compact_;
    Pass update_;
    Pass resolve_;

    bool     cpu_geometry_dirty_ = false;
    uint32_t scene_tri_count_ = 0;
    uint32_t scene_flags_     = 0;
    float    floor_y_         = 0.0f;
    float    scene_ray_far_   = 100.0f;
    float    cam_fwd_[4]      = {0, 0, -1, 1};
    float    cam_right_[4]    = {1, 0, 0, 1};
    float    cam_up_[4]       = {0, 1, 0, 0};
#endif

    bool initialized_ = false;
};

} // namespace pictor
