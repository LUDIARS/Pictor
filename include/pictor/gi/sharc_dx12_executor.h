#pragma once

/// SharcDx12Executor — SHaRC 拡張ライティングキャッシュの DirectX 12 実行。
///
/// `pictor::SharcGpuExecutor` (Vulkan 版, src/gi/sharc_executor.cpp) と同一
/// アルゴリズム・同一公開 API 契約の D3D12 移植。 GLSL 正本
/// (shaders/sharc/*.glsl) を HLSL (shaders/sharc/hlsl/*.hlsl, cs_5_1) へ
/// 忠実移植し、 D3DCompile (d3dcompiler_47) でランタイムコンパイルする
/// (dxc 不要)。
///
/// Vulkan 版との違い (デバイス寿命の非対称性):
///   Vulkan 版は外部の `VulkanContext&` を借用するのに対し、 本クラスは
///   D3D12 デバイス/キュー/フェンスを **自己所有** する (headless テスト
///   ターゲット single-purpose のため、 上位に既存の DX12 コンテキストが
///   まだ存在しないことによる設計判断)。
///
/// リソース割当 (root signature):
///   b0      : SharcParams (root CBV)
///   u0..u12 : UAV テーブル (keys/cells/rays/hits/counters/requests/stamps/
///             indirect/reservoirs/cell_pos/shade/output/予備)
///   t0..t5  : SRV テーブル (lights/bvh_nodes/tris/tri_mats/materials/atlas)
///   s0      : static sampler (LINEAR/WRAP/mip LINEAR)
///
/// SRP: D3D12 資源管理と dispatch 記録のみ。 セル表現の意味論は
/// `shaders/sharc/hlsl/*.hlsli` と `sharc_types.h`、 シーン生成は呼び出し側。

#include "pictor/core/types.h"
#include "pictor/gi/sharc_types.h"
#include "pictor/gi/sharc_executor.h"   // SharcConfig / SharcRayGpu / SharcShadeRequestGpu /
                                         // SharcLightGpu / SharcSceneUpload (API 非依存の値型)

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#endif

namespace pictor {

class SharcDx12Executor {
public:
    SharcDx12Executor() = default;
    ~SharcDx12Executor();

    SharcDx12Executor(const SharcDx12Executor&) = delete;
    SharcDx12Executor& operator=(const SharcDx12Executor&) = delete;

    bool is_initialized() const { return initialized_; }

#ifdef _WIN32
    /// D3D12 デバイス/キュー/資源を自己生成する。 `hlsl_shader_dir` は
    /// sharc_{hit,march,compact,update,resolve}.hlsl (+ .hlsli) の置き場所。
    /// `use_warp` = true で software adapter (WARP) を強制 (GPU なし環境の
    /// CI 用)。
    bool initialize(const std::string& hlsl_shader_dir, const SharcConfig& config,
                    bool use_warp = false);

    /// GPU シーン (BVH + 三角形 + マテリアル [+ アルベド配列]) を
    /// device-local バッファへ転送し、 hit パスを有効化する。 init 後に 1 回。
    bool upload_scene(const SharcSceneUpload& scene);

    void set_scene_floor(bool enabled, float floor_y);
    void set_scene_far(float ray_far);

    /// fov_scale = tan(fov/2)。 render_width はレイ index → 画素の変換用。
    void set_camera(const float3& fwd, const float3& right, const float3& up,
                    float fov_scale, float aspect, uint32_t render_width);

    /// フレーム開始: フレーム番号 / カメラを cbuffer へ反映する。
    void begin_frame(const float3& camera_pos);

    /// 今フレームの入力数を確定する (mapped 直書きの後に呼ぶ)。
    void set_counts(uint32_t ray_count, uint32_t light_count);

    /// 5 パス (hit + march/compact/update/resolve) の dispatch を
    /// 呼び出し側のコマンドリストへ記録する。 呼び出し側は record() の前に
    /// `cmd->SetPipelineState`/RootSignature 等を触らないこと (本関数が
    /// root signature からバインドする)。
    void record(ID3D12GraphicsCommandList* cmd);

    /// 同期実行 1 フレーム分のヘルパー: record() → ExecuteCommandLists →
    /// フェンス Signal + WaitForSingleObject。 テストドライバ用。
    void execute_and_wait(ID3D12GraphicsCommandList* cmd,
                          ID3D12CommandAllocator* allocator);

    // ---- ホスト直書き用 (D1 CPU 一次交差専用。 GPU シーン利用時は不要) ----
    SharcRayGpu*          rays_mapped();
    SharcShadeRequestGpu* shade_requests_mapped();
    SharcLightGpu*        lights_mapped();
    void commit_cpu_geometry() { cpu_geometry_dirty_ = true; }

    ID3D12Resource* output_buffer() const { return output_.resource.Get(); }
    uint64_t        output_size() const   { return output_.size; }

    /// 出力バッファ (device-local UAV) を READBACK ヒープへコピーし、
    /// CPU からマップして読める形にする (呼び出し側でフェンス待ち後に使う)。
    void copy_output_to_readback(ID3D12GraphicsCommandList* cmd);
    const void* map_readback();
    void unmap_readback();

    uint32_t request_count() const;
    uint32_t frame_index() const { return frame_index_; }

    ID3D12Device*       device() const { return device_.Get(); }
    ID3D12CommandQueue* queue() const  { return queue_.Get(); }
#endif

    void shutdown();

private:
#ifdef _WIN32
    struct Buffer {
        Microsoft::WRL::ComPtr<ID3D12Resource> resource;
        uint64_t size   = 0;
        uint32_t stride = 0;   // structured buffer element stride (0 = raw/uint)
        D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
        void* mapped = nullptr;
    };

    bool create_device_(bool use_warp);
    bool create_default_buffer_(Buffer& out, uint64_t size, uint32_t stride,
                                D3D12_RESOURCE_STATES initial_state);
    bool create_upload_buffer_(Buffer& out, uint64_t size, uint32_t stride);
    bool create_readback_buffer_(Buffer& out, uint64_t size);
    void zero_fill_(Buffer& target, ID3D12GraphicsCommandList* cmd);
    bool upload_to_default_(Buffer& out, const void* data, uint64_t size,
                            uint32_t stride, D3D12_RESOURCE_STATES final_state,
                            ID3D12GraphicsCommandList* cmd);
    void transition_(ID3D12GraphicsCommandList* cmd, Buffer& b,
                     D3D12_RESOURCE_STATES to);
    void uav_barrier_(ID3D12GraphicsCommandList* cmd, ID3D12Resource* res);

    bool create_root_signature_();
    bool compile_and_create_pso_(Microsoft::WRL::ComPtr<ID3D12PipelineState>& out,
                                 const std::wstring& hlsl_path);
    bool create_descriptor_heap_and_views_();
    void write_uav_(uint32_t slot, Buffer& b, uint32_t num_elements);
    void write_srv_buffer_(uint32_t slot, Buffer& b, uint32_t num_elements);
    void write_srv_texture_(uint32_t slot, ID3D12Resource* tex, uint32_t layers,
                            uint32_t mips);
    bool create_atlas_(const uint8_t* pixels, uint32_t size, uint32_t layers,
                       ID3D12GraphicsCommandList* cmd);
    void write_params_();
    bool run_one_shot_(const std::function<void(ID3D12GraphicsCommandList*)>& fn);

    // run_one_shot_ 実行中に生成する一時 UPLOAD リソース (GPU 完了待ちまで
    // 生存させる必要がある — フェンス wait 後に run_one_shot_ が clear する)。
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> temp_uploads_;

    Microsoft::WRL::ComPtr<IDXGIFactory6> factory_;
    Microsoft::WRL::ComPtr<ID3D12Device>  device_;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue>     queue_;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> init_allocator_;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> init_cmd_;
    Microsoft::WRL::ComPtr<ID3D12Fence> fence_;
    uint64_t   fence_value_ = 0;
    HANDLE     fence_event_ = nullptr;

    SharcConfig config_{};
    uint32_t    ray_count_   = 0;
    uint32_t    light_count_ = 0;
    uint32_t    frame_index_ = 0;
    float3      camera_pos_{};

    Buffer params_;        // b0 (root CBV, UPLOAD, mapped)
    Buffer keys_;          // u0
    Buffer cells_;         // u1
    Buffer rays_;          // u2 (DEFAULT)
    Buffer hits_;          // u3
    Buffer counters_;      // u4
    Buffer requests_;      // u5
    Buffer stamps_;        // u6
    Buffer indirect_;      // u7 (also ExecuteIndirect argument buffer)
    Buffer reservoirs_;    // u8
    Buffer cell_pos_;      // u9
    Buffer shade_;         // u10 (DEFAULT)
    Buffer output_;        // u11
    Buffer u12_dummy_;     // u12 (予備、未使用)

    Buffer lights_;          // t0 (UPLOAD, mapped, 直接 SRV)
    Buffer scene_nodes_;     // t1
    Buffer scene_tris_;      // t2
    Buffer scene_tri_mats_;  // t3
    Buffer scene_materials_; // t4

    Buffer rays_staging_;    // D1 CPU 経路の転送元 (UPLOAD, mapped)
    Buffer shade_staging_;   // 同上
    Buffer counters_rb_;     // request_count 読み出し用 (READBACK)
    Buffer output_rb_;       // output readback (テストドライバ用)
    Buffer zero_counters_;   // per-frame counters クリア用 (UPLOAD, 16B, ゼロ)

    // アルベドテクスチャ配列 (t5、未使用時は 1x1 白ダミー)
    Microsoft::WRL::ComPtr<ID3D12Resource> atlas_;
    uint32_t atlas_layers_ = 0;
    uint32_t atlas_mips_   = 1;

    Microsoft::WRL::ComPtr<ID3D12RootSignature> root_signature_;
    Microsoft::WRL::ComPtr<ID3D12CommandSignature> dispatch_indirect_sig_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pso_hit_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pso_march_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pso_compact_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pso_update_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pso_resolve_;

    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heap_;       // shader-visible CBV_SRV_UAV
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> sampler_heap_;
    uint32_t descriptor_size_ = 0;

    std::string hlsl_dir_;
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
