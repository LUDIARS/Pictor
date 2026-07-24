#include "pictor/gi/sharc_dx12_executor.h"

#ifdef _WIN32
#include <d3dcompiler.h>
#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#endif

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace pictor {

SharcDx12Executor::~SharcDx12Executor() {
    shutdown();
}

#ifdef _WIN32

using Microsoft::WRL::ComPtr;

namespace {

constexpr uint32_t kUavCount = 13;   // u0..u12
constexpr uint32_t kSrvCount = 6;    // t0..t5
constexpr uint32_t kHeapCount = kUavCount + kSrvCount;

std::wstring widen(const std::string& s) {
    std::wstring w(s.begin(), s.end());
    return w;
}

} // namespace

// ============================================================
// デバイス / 基盤資源
// ============================================================

bool SharcDx12Executor::create_device_(bool use_warp) {
    UINT factory_flags = 0;
#ifndef NDEBUG
    {
        ComPtr<ID3D12Debug> debug;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) {
            debug->EnableDebugLayer();
        }
    }
    factory_flags |= DXGI_CREATE_FACTORY_DEBUG;
#endif
    if (FAILED(CreateDXGIFactory2(factory_flags, IID_PPV_ARGS(&factory_)))) {
        std::fprintf(stderr, "[sharc-dx12] CreateDXGIFactory2 failed\n");
        return false;
    }

    ComPtr<IDXGIAdapter1> adapter;
    if (use_warp) {
        if (FAILED(factory_->EnumWarpAdapter(IID_PPV_ARGS(&adapter)))) {
            std::fprintf(stderr, "[sharc-dx12] EnumWarpAdapter failed\n");
            return false;
        }
    } else {
        for (UINT i = 0; factory_->EnumAdapterByGpuPreference(
                             i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                             IID_PPV_ARGS(&adapter)) != DXGI_ERROR_NOT_FOUND;
             ++i) {
            DXGI_ADAPTER_DESC1 desc{};
            adapter->GetDesc1(&desc);
            if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
            if (SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0,
                                            _uuidof(ID3D12Device), nullptr))) {
                break;
            }
            adapter.Reset();
        }
        if (!adapter) {
            // ハードウェアアダプタが見つからない → WARP へフォールバック
            std::fprintf(stderr,
                         "[sharc-dx12] no hardware adapter — falling back to WARP\n");
            if (FAILED(factory_->EnumWarpAdapter(IID_PPV_ARGS(&adapter)))) {
                std::fprintf(stderr, "[sharc-dx12] EnumWarpAdapter failed\n");
                return false;
            }
        }
    }

    if (FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0,
                                 IID_PPV_ARGS(&device_)))) {
        std::fprintf(stderr, "[sharc-dx12] D3D12CreateDevice failed\n");
        return false;
    }

    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(device_->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue_)))) {
        std::fprintf(stderr, "[sharc-dx12] CreateCommandQueue failed\n");
        return false;
    }
    if (FAILED(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                               IID_PPV_ARGS(&init_allocator_)))) {
        return false;
    }
    if (FAILED(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                          init_allocator_.Get(), nullptr,
                                          IID_PPV_ARGS(&init_cmd_)))) {
        return false;
    }
    init_cmd_->Close();

    if (FAILED(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                                    IID_PPV_ARGS(&fence_)))) {
        return false;
    }
    fence_event_ = CreateEventExW(nullptr, nullptr, 0, EVENT_ALL_ACCESS);
    if (!fence_event_) return false;

    descriptor_size_ = device_->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    return true;
}

bool SharcDx12Executor::run_one_shot_(
    const std::function<void(ID3D12GraphicsCommandList*)>& fn) {
    if (FAILED(init_allocator_->Reset())) return false;
    if (FAILED(init_cmd_->Reset(init_allocator_.Get(), nullptr))) return false;
    fn(init_cmd_.Get());
    if (FAILED(init_cmd_->Close())) return false;
    ID3D12CommandList* lists[] = {init_cmd_.Get()};
    queue_->ExecuteCommandLists(1, lists);
    ++fence_value_;
    queue_->Signal(fence_.Get(), fence_value_);
    if (fence_->GetCompletedValue() < fence_value_) {
        fence_->SetEventOnCompletion(fence_value_, fence_event_);
        WaitForSingleObject(fence_event_, INFINITE);
    }
    temp_uploads_.clear();
    return true;
}

// ============================================================
// バッファ / リソース
// ============================================================

bool SharcDx12Executor::create_default_buffer_(Buffer& out, uint64_t size,
                                               uint32_t stride,
                                               D3D12_RESOURCE_STATES initial_state) {
    out = Buffer{};
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width             = (std::max)(size, uint64_t{4});
    desc.Height             = 1;
    desc.DepthOrArraySize   = 1;
    desc.MipLevels          = 1;
    desc.Format             = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count   = 1;
    desc.Layout             = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags              = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    if (FAILED(device_->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &desc, initial_state, nullptr,
            IID_PPV_ARGS(&out.resource)))) {
        std::fprintf(stderr, "[sharc-dx12] CreateCommittedResource (DEFAULT) failed\n");
        return false;
    }
    out.size   = desc.Width;
    out.stride = stride;
    out.state  = initial_state;
    return true;
}

bool SharcDx12Executor::create_upload_buffer_(Buffer& out, uint64_t size,
                                              uint32_t stride) {
    out = Buffer{};
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension       = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width           = (std::max)(size, uint64_t{4});
    desc.Height           = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels        = 1;
    desc.Format           = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1;
    desc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(device_->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr, IID_PPV_ARGS(&out.resource)))) {
        std::fprintf(stderr, "[sharc-dx12] CreateCommittedResource (UPLOAD) failed\n");
        return false;
    }
    out.size   = desc.Width;
    out.stride = stride;
    out.state  = D3D12_RESOURCE_STATE_GENERIC_READ;
    D3D12_RANGE no_read{0, 0};
    if (FAILED(out.resource->Map(0, &no_read, &out.mapped))) return false;
    std::memset(out.mapped, 0, static_cast<size_t>(out.size));
    return true;
}

bool SharcDx12Executor::create_readback_buffer_(Buffer& out, uint64_t size) {
    out = Buffer{};
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension       = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width           = (std::max)(size, uint64_t{4});
    desc.Height           = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels        = 1;
    desc.Format           = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1;
    desc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(device_->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr, IID_PPV_ARGS(&out.resource)))) {
        std::fprintf(stderr, "[sharc-dx12] CreateCommittedResource (READBACK) failed\n");
        return false;
    }
    out.size  = desc.Width;
    out.state = D3D12_RESOURCE_STATE_COPY_DEST;
    return true;
}

void SharcDx12Executor::transition_(ID3D12GraphicsCommandList* cmd, Buffer& b,
                                    D3D12_RESOURCE_STATES to) {
    if (b.state == to) return;
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type  = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource   = b.resource.Get();
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = b.state;
    barrier.Transition.StateAfter  = to;
    cmd->ResourceBarrier(1, &barrier);
    b.state = to;
}

void SharcDx12Executor::uav_barrier_(ID3D12GraphicsCommandList* cmd,
                                     ID3D12Resource* res) {
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type          = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barrier.UAV.pResource = res;
    cmd->ResourceBarrier(1, &barrier);
}

void SharcDx12Executor::zero_fill_(Buffer& target, ID3D12GraphicsCommandList* cmd) {
    Buffer upload;
    if (!create_upload_buffer_(upload, target.size, 0)) return;   // 既にゼロ埋め済み
    ComPtr<ID3D12Resource> keep = upload.resource;
    transition_(cmd, target, D3D12_RESOURCE_STATE_COPY_DEST);
    cmd->CopyBufferRegion(target.resource.Get(), 0, upload.resource.Get(), 0,
                          target.size);
    transition_(cmd, target, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    temp_uploads_.push_back(keep);
}

bool SharcDx12Executor::upload_to_default_(Buffer& out, const void* data,
                                           uint64_t size, uint32_t stride,
                                           D3D12_RESOURCE_STATES final_state,
                                           ID3D12GraphicsCommandList* cmd) {
    Buffer upload;
    if (!create_upload_buffer_(upload, size, 0)) return false;
    std::memcpy(upload.mapped, data, static_cast<size_t>(size));
    ComPtr<ID3D12Resource> keep = upload.resource;
    transition_(cmd, out, D3D12_RESOURCE_STATE_COPY_DEST);
    cmd->CopyBufferRegion(out.resource.Get(), 0, upload.resource.Get(), 0, size);
    transition_(cmd, out, final_state);
    out.stride = stride;
    temp_uploads_.push_back(keep);
    return true;
}

// ============================================================
// ディスクリプタ / ビュー
// ============================================================

void SharcDx12Executor::write_uav_(uint32_t slot, Buffer& b, uint32_t num_elements) {
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
    uav.ViewDimension               = D3D12_UAV_DIMENSION_BUFFER;
    uav.Format                      = DXGI_FORMAT_UNKNOWN;
    uav.Buffer.FirstElement         = 0;
    uav.Buffer.NumElements          = num_elements;
    uav.Buffer.StructureByteStride  = b.stride;
    uav.Buffer.Flags                = D3D12_BUFFER_UAV_FLAG_NONE;
    D3D12_CPU_DESCRIPTOR_HANDLE h = heap_->GetCPUDescriptorHandleForHeapStart();
    h.ptr += static_cast<SIZE_T>(slot) * descriptor_size_;
    device_->CreateUnorderedAccessView(b.resource.Get(), nullptr, &uav, h);
}

void SharcDx12Executor::write_srv_buffer_(uint32_t slot, Buffer& b,
                                          uint32_t num_elements) {
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.ViewDimension               = D3D12_SRV_DIMENSION_BUFFER;
    srv.Format                      = DXGI_FORMAT_UNKNOWN;
    srv.Shader4ComponentMapping     = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Buffer.FirstElement         = 0;
    srv.Buffer.NumElements          = num_elements;
    srv.Buffer.StructureByteStride  = b.stride;
    srv.Buffer.Flags                = D3D12_BUFFER_SRV_FLAG_NONE;
    D3D12_CPU_DESCRIPTOR_HANDLE h = heap_->GetCPUDescriptorHandleForHeapStart();
    h.ptr += static_cast<SIZE_T>(slot) * descriptor_size_;
    device_->CreateShaderResourceView(b.resource.Get(), &srv, h);
}

void SharcDx12Executor::write_srv_texture_(uint32_t slot, ID3D12Resource* tex,
                                           uint32_t layers, uint32_t mips) {
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.ViewDimension                        = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
    srv.Format                               = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    srv.Shader4ComponentMapping              = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2DArray.MostDetailedMip       = 0;
    srv.Texture2DArray.MipLevels             = mips;
    srv.Texture2DArray.FirstArraySlice       = 0;
    srv.Texture2DArray.ArraySize             = layers;
    D3D12_CPU_DESCRIPTOR_HANDLE h = heap_->GetCPUDescriptorHandleForHeapStart();
    h.ptr += static_cast<SIZE_T>(slot) * descriptor_size_;
    device_->CreateShaderResourceView(tex, &srv, h);
}

bool SharcDx12Executor::create_atlas_(const uint8_t* pixels, uint32_t size,
                                      uint32_t layers,
                                      ID3D12GraphicsCommandList* cmd) {
    atlas_.Reset();
    atlas_layers_ = layers;
    atlas_mips_   = 1;   // 簡略化: ミップ生成なし (品質差は許容, spec 指示)

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension         = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width             = size;
    desc.Height            = size;
    desc.DepthOrArraySize  = static_cast<UINT16>(layers);
    desc.MipLevels         = 1;
    desc.Format            = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    desc.SampleDesc.Count  = 1;
    desc.Layout            = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    if (FAILED(device_->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr, IID_PPV_ARGS(&atlas_)))) {
        std::fprintf(stderr, "[sharc-dx12] atlas CreateCommittedResource failed\n");
        return false;
    }

    const UINT row_pitch = (size * 4u + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1u) &
                           ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1u);
    const UINT64 layer_bytes = static_cast<UINT64>(row_pitch) * size;
    const UINT64 total_bytes = layer_bytes * layers;

    Buffer upload;
    if (!create_upload_buffer_(upload, total_bytes, 0)) return false;
    auto* dst = static_cast<uint8_t*>(upload.mapped);
    for (uint32_t layer = 0; layer < layers; ++layer) {
        const uint8_t* src_layer = pixels + static_cast<size_t>(layer) * size * size * 4u;
        uint8_t* dst_layer = dst + layer * layer_bytes;
        for (uint32_t row = 0; row < size; ++row) {
            std::memcpy(dst_layer + row * row_pitch, src_layer + row * size * 4u,
                       size * 4u);
        }
    }
    ComPtr<ID3D12Resource> keep = upload.resource;
    temp_uploads_.push_back(keep);

    for (uint32_t layer = 0; layer < layers; ++layer) {
        D3D12_TEXTURE_COPY_LOCATION dstLoc{};
        dstLoc.pResource        = atlas_.Get();
        dstLoc.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dstLoc.SubresourceIndex = layer;   // 1 mip → subresource = layer

        D3D12_TEXTURE_COPY_LOCATION srcLoc{};
        srcLoc.pResource = upload.resource.Get();
        srcLoc.Type      = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        srcLoc.PlacedFootprint.Offset             = layer * layer_bytes;
        srcLoc.PlacedFootprint.Footprint.Format   = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        srcLoc.PlacedFootprint.Footprint.Width    = size;
        srcLoc.PlacedFootprint.Footprint.Height   = size;
        srcLoc.PlacedFootprint.Footprint.Depth    = 1;
        srcLoc.PlacedFootprint.Footprint.RowPitch = row_pitch;

        cmd->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, nullptr);
    }

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource   = atlas_.Get();
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    cmd->ResourceBarrier(1, &barrier);

    write_srv_texture_(18, atlas_.Get(), layers, 1);
    return true;
}

// ============================================================
// root signature / PSO
// ============================================================

bool SharcDx12Executor::create_root_signature_() {
    D3D12_DESCRIPTOR_RANGE uav_range{};
    uav_range.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    uav_range.NumDescriptors                    = kUavCount;
    uav_range.BaseShaderRegister                = 0;
    uav_range.RegisterSpace                     = 0;
    uav_range.OffsetInDescriptorsFromTableStart = 0;

    D3D12_DESCRIPTOR_RANGE srv_range{};
    srv_range.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srv_range.NumDescriptors                    = kSrvCount;
    srv_range.BaseShaderRegister                = 0;
    srv_range.RegisterSpace                     = 0;
    srv_range.OffsetInDescriptorsFromTableStart = 0;

    D3D12_ROOT_PARAMETER params[3]{};
    params[0].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[0].Descriptor.ShaderRegister = 0;
    params[0].Descriptor.RegisterSpace  = 0;
    params[0].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;

    params[1].ParameterType    = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 1;
    params[1].DescriptorTable.pDescriptorRanges   = &uav_range;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    params[2].ParameterType    = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[2].DescriptorTable.NumDescriptorRanges = 1;
    params[2].DescriptorTable.pDescriptorRanges   = &srv_range;
    params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_STATIC_SAMPLER_DESC samp{};
    samp.Filter           = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    samp.AddressU         = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    samp.AddressV         = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    samp.AddressW         = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    samp.ComparisonFunc   = D3D12_COMPARISON_FUNC_NEVER;
    samp.MinLOD           = 0.0f;
    samp.MaxLOD           = D3D12_FLOAT32_MAX;
    samp.ShaderRegister   = 0;
    samp.RegisterSpace    = 0;
    samp.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC desc{};
    desc.NumParameters     = 3;
    desc.pParameters       = params;
    desc.NumStaticSamplers = 1;
    desc.pStaticSamplers   = &samp;
    desc.Flags             = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    ComPtr<ID3DBlob> blob, error;
    HRESULT hr = D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1,
                                             &blob, &error);
    if (FAILED(hr)) {
        if (error) {
            std::fprintf(stderr, "[sharc-dx12] root signature: %s\n",
                        static_cast<const char*>(error->GetBufferPointer()));
        }
        return false;
    }
    return SUCCEEDED(device_->CreateRootSignature(
        0, blob->GetBufferPointer(), blob->GetBufferSize(),
        IID_PPV_ARGS(&root_signature_)));
}

bool SharcDx12Executor::compile_and_create_pso_(
    ComPtr<ID3D12PipelineState>& out, const std::wstring& hlsl_path) {
    UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#ifndef NDEBUG
    flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif
    ComPtr<ID3DBlob> code, error;
    HRESULT hr = D3DCompileFromFile(hlsl_path.c_str(), nullptr,
                                    D3D_COMPILE_STANDARD_FILE_INCLUDE, "main",
                                    "cs_5_1", flags, 0, &code, &error);
    if (FAILED(hr)) {
        std::fprintf(stderr, "[sharc-dx12] compile failed: %ls\n%s\n",
                    hlsl_path.c_str(),
                    error ? static_cast<const char*>(error->GetBufferPointer())
                          : "(no error blob)");
        return false;
    }
    D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = root_signature_.Get();
    pso.CS             = {code->GetBufferPointer(), code->GetBufferSize()};
    if (FAILED(device_->CreateComputePipelineState(&pso, IID_PPV_ARGS(&out)))) {
        std::fprintf(stderr, "[sharc-dx12] CreateComputePipelineState failed: %ls\n",
                    hlsl_path.c_str());
        return false;
    }
    return true;
}

bool SharcDx12Executor::create_descriptor_heap_and_views_() {
    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = kHeapCount;
    hd.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(device_->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap_)))) {
        std::fprintf(stderr, "[sharc-dx12] CreateDescriptorHeap failed\n");
        return false;
    }
    return true;
}

// ============================================================
// パラメータ
// ============================================================

void SharcDx12Executor::write_params_() {
    SharcParamsGpu p{};
    p.camera_pos[0]  = camera_pos_.x;
    p.camera_pos[1]  = camera_pos_.y;
    p.camera_pos[2]  = camera_pos_.z;
    p.base_cell_size = config_.base_cell_size;
    p.level_count    = config_.level_count;
    p.table_size     = config_.table_size;
    p.frame_index    = frame_index_;
    p.ema_alpha      = config_.ema_alpha;
    p.level_bias     = config_.level_bias;
    p.sss_mfp_scale  = config_.sss_mfp_scale;
    p.max_ray_steps  = config_.max_ray_steps;
    p.light_count    = light_count_;
    p.ray_count      = ray_count_;
    p.stale_frames   = config_.stale_frames;
    p.hit_epsilon    = config_.hit_epsilon;
    p.scene_tri_count = scene_tri_count_;
    p.scene_flags     = scene_flags_;
    p.floor_y         = floor_y_;
    p.scene_ray_far   = scene_ray_far_;
    std::memcpy(p.cam_fwd, cam_fwd_, sizeof(cam_fwd_));
    std::memcpy(p.cam_right, cam_right_, sizeof(cam_right_));
    std::memcpy(p.cam_up, cam_up_, sizeof(cam_up_));
    std::memcpy(params_.mapped, &p, sizeof(p));
}

void SharcDx12Executor::set_camera(const float3& fwd, const float3& right,
                                   const float3& up, float fov_scale,
                                   float aspect, uint32_t render_width) {
    cam_fwd_[0] = fwd.x;   cam_fwd_[1] = fwd.y;   cam_fwd_[2] = fwd.z;
    cam_fwd_[3] = fov_scale;
    cam_right_[0] = right.x; cam_right_[1] = right.y; cam_right_[2] = right.z;
    cam_right_[3] = aspect;
    cam_up_[0] = up.x; cam_up_[1] = up.y; cam_up_[2] = up.z;
    cam_up_[3] = static_cast<float>(render_width);
}

// ============================================================
// initialize / upload_scene / shutdown
// ============================================================

bool SharcDx12Executor::initialize(const std::string& hlsl_shader_dir,
                                   const SharcConfig& config, bool use_warp) {
    hlsl_dir_ = hlsl_shader_dir;
    config_   = config;

    if ((config_.table_size & (config_.table_size - 1)) != 0) {
        std::fprintf(stderr, "[sharc-dx12] table_size must be a power of two\n");
        return false;
    }
    if (!create_device_(use_warp)) return false;
    if (!create_descriptor_heap_and_views_()) return false;
    if (!create_root_signature_()) return false;

    const auto dir = hlsl_dir_;
    if (!compile_and_create_pso_(pso_hit_, widen(dir + "/sharc_hit.hlsl")) ||
        !compile_and_create_pso_(pso_march_, widen(dir + "/sharc_march.hlsl")) ||
        !compile_and_create_pso_(pso_compact_, widen(dir + "/sharc_compact.hlsl")) ||
        !compile_and_create_pso_(pso_update_, widen(dir + "/sharc_update.hlsl")) ||
        !compile_and_create_pso_(pso_resolve_, widen(dir + "/sharc_resolve.hlsl"))) {
        return false;
    }

    // ExecuteIndirect command signature (Dispatch のみ → root signature 不要)
    D3D12_INDIRECT_ARGUMENT_DESC arg{};
    arg.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;
    D3D12_COMMAND_SIGNATURE_DESC cs{};
    cs.ByteStride       = sizeof(D3D12_DISPATCH_ARGUMENTS);
    cs.NumArgumentDescs = 1;
    cs.pArgumentDescs   = &arg;
    if (FAILED(device_->CreateCommandSignature(&cs, nullptr,
                                               IID_PPV_ARGS(&dispatch_indirect_sig_)))) {
        std::fprintf(stderr, "[sharc-dx12] CreateCommandSignature failed\n");
        return false;
    }

    // ── バッファ確保 ──
    const uint64_t slots = config_.table_size;
    const uint64_t rays_bytes  = uint64_t{config_.max_rays} * sizeof(SharcRayGpu);
    const uint64_t shade_bytes = uint64_t{config_.max_rays} * sizeof(SharcShadeRequestGpu);

    if (!create_upload_buffer_(params_, sizeof(SharcParamsGpu), 0)) return false;
    if (!create_default_buffer_(keys_, slots * 4, 4, D3D12_RESOURCE_STATE_COPY_DEST) ||
        !create_default_buffer_(cells_, slots * kSharcCellBytes, 4, D3D12_RESOURCE_STATE_COPY_DEST) ||
        !create_default_buffer_(rays_, rays_bytes, sizeof(SharcRayGpu), D3D12_RESOURCE_STATE_COPY_DEST) ||
        !create_default_buffer_(hits_, uint64_t{config_.max_hits} * kSharcHitRecordBytes,
                               kSharcHitRecordBytes, D3D12_RESOURCE_STATE_COPY_DEST) ||
        !create_default_buffer_(counters_, 4 * 4, 4, D3D12_RESOURCE_STATE_COPY_DEST) ||
        !create_default_buffer_(requests_, slots * 4, 4, D3D12_RESOURCE_STATE_COPY_DEST) ||
        !create_default_buffer_(stamps_, slots * 4, 4, D3D12_RESOURCE_STATE_COPY_DEST) ||
        !create_default_buffer_(indirect_, 3 * 4, 4, D3D12_RESOURCE_STATE_COPY_DEST) ||
        !create_default_buffer_(reservoirs_, slots * kSharcReservoirUints * 4, 16,
                               D3D12_RESOURCE_STATE_COPY_DEST) ||
        !create_default_buffer_(cell_pos_, slots * 8, 8, D3D12_RESOURCE_STATE_COPY_DEST) ||
        !create_default_buffer_(shade_, shade_bytes, sizeof(SharcShadeRequestGpu),
                               D3D12_RESOURCE_STATE_COPY_DEST) ||
        !create_default_buffer_(output_, uint64_t{config_.max_rays} * 16, 16,
                               D3D12_RESOURCE_STATE_COPY_DEST) ||
        !create_default_buffer_(u12_dummy_, 4, 4, D3D12_RESOURCE_STATE_COPY_DEST)) {
        std::fprintf(stderr, "[sharc-dx12] buffer creation failed\n");
        return false;
    }
    // シーン (upload_scene まではプレースホルダ、SRV の有効性のため最小確保)
    if (!create_default_buffer_(scene_nodes_, 32, sizeof(SharcBvhNodeGpu),
                               D3D12_RESOURCE_STATE_COPY_DEST) ||
        !create_default_buffer_(scene_tris_, 80, sizeof(SharcTriGpu),
                               D3D12_RESOURCE_STATE_COPY_DEST) ||
        !create_default_buffer_(scene_tri_mats_, 4, 4, D3D12_RESOURCE_STATE_COPY_DEST) ||
        !create_default_buffer_(scene_materials_, 32, sizeof(SharcMaterialGpu),
                               D3D12_RESOURCE_STATE_COPY_DEST)) {
        return false;
    }
    if (!create_upload_buffer_(lights_, uint64_t{config_.max_lights} * sizeof(SharcLightGpu),
                              sizeof(SharcLightGpu))) {
        return false;
    }
    if (!create_upload_buffer_(rays_staging_, rays_bytes, 0) ||
        !create_upload_buffer_(shade_staging_, shade_bytes, 0) ||
        !create_upload_buffer_(zero_counters_, 4 * 4, 0)) {
        return false;
    }
    if (!create_readback_buffer_(counters_rb_, 4 * 4)) return false;
    {
        D3D12_RANGE no_read{0, 0};
        if (FAILED(counters_rb_.resource->Map(0, &no_read, &counters_rb_.mapped)))
            return false;
    }
    if (!create_readback_buffer_(output_rb_, uint64_t{config_.max_rays} * 16)) return false;

    // ── ゼロ初期化 + プレースホルダ SRV/UAV 書き込み + 1x1 白アルベド ──
    bool one_shot_ok = true;
    const bool ros_ok = run_one_shot_([&](ID3D12GraphicsCommandList* cmd) {
        for (Buffer* b : {&keys_, &cells_, &rays_, &hits_, &counters_, &requests_,
                          &stamps_, &indirect_, &reservoirs_, &cell_pos_, &shade_,
                          &output_, &u12_dummy_}) {
            zero_fill_(*b, cmd);
        }
        transition_(cmd, scene_nodes_, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        transition_(cmd, scene_tris_, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        transition_(cmd, scene_tri_mats_, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        transition_(cmd, scene_materials_, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        const uint8_t white[4] = {255, 255, 255, 255};
        one_shot_ok = create_atlas_(white, 1, 1, cmd);
    });
    if (!ros_ok || !one_shot_ok) return false;

    // ── ディスクリプタテーブル書き込み ──
    write_uav_(0, keys_, static_cast<uint32_t>(slots));
    write_uav_(1, cells_, static_cast<uint32_t>(slots * kSharcCellUints));
    write_uav_(2, rays_, config_.max_rays);
    write_uav_(3, hits_, config_.max_hits);
    write_uav_(4, counters_, 4);
    write_uav_(5, requests_, static_cast<uint32_t>(slots));
    write_uav_(6, stamps_, static_cast<uint32_t>(slots));
    write_uav_(7, indirect_, 3);
    write_uav_(8, reservoirs_, static_cast<uint32_t>(slots));
    write_uav_(9, cell_pos_, static_cast<uint32_t>(slots));
    write_uav_(10, shade_, config_.max_rays);
    write_uav_(11, output_, config_.max_rays);
    write_uav_(12, u12_dummy_, 1);

    write_srv_buffer_(13, lights_, config_.max_lights);
    write_srv_buffer_(14, scene_nodes_, 1);
    write_srv_buffer_(15, scene_tris_, 1);
    write_srv_buffer_(16, scene_tri_mats_, 1);
    write_srv_buffer_(17, scene_materials_, 1);
    // slot 18 (atlas) は create_atlas_ が書き込み済み

    write_params_();
    initialized_ = true;
    std::fprintf(stderr,
                "[sharc-dx12] ready: %u slots (%.1f MB cells), %u rays, %u lights\n",
                config_.table_size,
                static_cast<double>(slots * kSharcCellBytes) / (1024.0 * 1024.0),
                config_.max_rays, config_.max_lights);
    return true;
}

bool SharcDx12Executor::upload_scene(const SharcSceneUpload& scene) {
    if (!initialized_ || scene.node_count == 0 || scene.tri_count == 0) return false;

    bool ok = true;
    const bool ros_ok = run_one_shot_([&](ID3D12GraphicsCommandList* cmd) {
        ok &= create_default_buffer_(scene_nodes_,
                                     uint64_t{scene.node_count} * sizeof(SharcBvhNodeGpu),
                                     sizeof(SharcBvhNodeGpu), D3D12_RESOURCE_STATE_COPY_DEST);
        ok &= create_default_buffer_(scene_tris_,
                                     uint64_t{scene.tri_count} * sizeof(SharcTriGpu),
                                     sizeof(SharcTriGpu), D3D12_RESOURCE_STATE_COPY_DEST);
        ok &= create_default_buffer_(scene_tri_mats_, uint64_t{scene.tri_count} * 4, 4,
                                     D3D12_RESOURCE_STATE_COPY_DEST);
        ok &= create_default_buffer_(scene_materials_,
                                     uint64_t{scene.material_count} * sizeof(SharcMaterialGpu),
                                     sizeof(SharcMaterialGpu), D3D12_RESOURCE_STATE_COPY_DEST);
        if (!ok) return;
        ok &= upload_to_default_(scene_nodes_, scene.nodes,
                                 uint64_t{scene.node_count} * sizeof(SharcBvhNodeGpu),
                                 sizeof(SharcBvhNodeGpu),
                                 D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, cmd);
        ok &= upload_to_default_(scene_tris_, scene.tris,
                                 uint64_t{scene.tri_count} * sizeof(SharcTriGpu),
                                 sizeof(SharcTriGpu),
                                 D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, cmd);
        ok &= upload_to_default_(scene_tri_mats_, scene.tri_materials,
                                 uint64_t{scene.tri_count} * 4, 4,
                                 D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, cmd);
        ok &= upload_to_default_(scene_materials_, scene.materials,
                                 uint64_t{scene.material_count} * sizeof(SharcMaterialGpu),
                                 sizeof(SharcMaterialGpu),
                                 D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, cmd);
        if (scene.atlas_pixels != nullptr && scene.atlas_layers > 0) {
            ok &= create_atlas_(scene.atlas_pixels, scene.atlas_size,
                               scene.atlas_layers, cmd);
        }
    });
    if (!ros_ok || !ok) {
        std::fprintf(stderr, "[sharc-dx12] scene upload failed\n");
        return false;
    }

    write_srv_buffer_(14, scene_nodes_, scene.node_count);
    write_srv_buffer_(15, scene_tris_, scene.tri_count);
    write_srv_buffer_(16, scene_tri_mats_, scene.tri_count);
    write_srv_buffer_(17, scene_materials_, scene.material_count);

    scene_tri_count_ = scene.tri_count;
    scene_flags_ |= kSharcSceneMesh;
    write_params_();
    std::fprintf(stderr,
                "[sharc-dx12] scene uploaded: %u nodes, %u tris (%.1f MB device-local)\n",
                scene.node_count, scene.tri_count,
                static_cast<double>(scene_nodes_.size + scene_tris_.size +
                                    scene_tri_mats_.size + scene_materials_.size) /
                    (1024.0 * 1024.0));
    return true;
}

void SharcDx12Executor::set_scene_floor(bool enabled, float floor_y) {
    if (enabled) scene_flags_ |= kSharcSceneFloor;
    else         scene_flags_ &= ~kSharcSceneFloor;
    floor_y_ = floor_y;
    if (initialized_) write_params_();
}

void SharcDx12Executor::set_scene_far(float ray_far) {
    scene_ray_far_ = ray_far;
    if (initialized_) write_params_();
}

void SharcDx12Executor::begin_frame(const float3& camera_pos) {
    if (!initialized_) return;
    camera_pos_ = camera_pos;
    ++frame_index_;
    write_params_();
}

void SharcDx12Executor::set_counts(uint32_t ray_count, uint32_t light_count) {
    if (!initialized_) return;
    ray_count_   = (std::min)(ray_count, config_.max_rays);
    light_count_ = (std::min)(light_count, config_.max_lights);
    write_params_();
}

SharcRayGpu* SharcDx12Executor::rays_mapped() {
    return static_cast<SharcRayGpu*>(rays_staging_.mapped);
}
SharcShadeRequestGpu* SharcDx12Executor::shade_requests_mapped() {
    return static_cast<SharcShadeRequestGpu*>(shade_staging_.mapped);
}
SharcLightGpu* SharcDx12Executor::lights_mapped() {
    return static_cast<SharcLightGpu*>(lights_.mapped);
}

uint32_t SharcDx12Executor::request_count() const {
    if (!initialized_) return 0;
    return static_cast<const uint32_t*>(counters_rb_.mapped)[1];
}

// ============================================================
// フレーム記録
// ============================================================

void SharcDx12Executor::record(ID3D12GraphicsCommandList* cmd) {
    if (!initialized_ || ray_count_ == 0) return;

    // ── per-frame カウンタのクリア ──
    transition_(cmd, counters_, D3D12_RESOURCE_STATE_COPY_DEST);
    cmd->CopyBufferRegion(counters_.resource.Get(), 0, zero_counters_.resource.Get(),
                          0, counters_.size);
    transition_(cmd, counters_, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    // ── D1 (CPU 一次交差) の staging → 本体転送 (dirty 時のみ) ──
    if (cpu_geometry_dirty_) {
        transition_(cmd, rays_, D3D12_RESOURCE_STATE_COPY_DEST);
        cmd->CopyBufferRegion(rays_.resource.Get(), 0, rays_staging_.resource.Get(), 0,
                              rays_.size);
        transition_(cmd, rays_, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        transition_(cmd, shade_, D3D12_RESOURCE_STATE_COPY_DEST);
        cmd->CopyBufferRegion(shade_.resource.Get(), 0, shade_staging_.resource.Get(), 0,
                              shade_.size);
        transition_(cmd, shade_, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        cpu_geometry_dirty_ = false;
    }

    cmd->SetComputeRootSignature(root_signature_.Get());
    ID3D12DescriptorHeap* heaps[] = {heap_.Get()};
    cmd->SetDescriptorHeaps(1, heaps);
    cmd->SetComputeRootConstantBufferView(0, params_.resource->GetGPUVirtualAddress());
    D3D12_GPU_DESCRIPTOR_HANDLE uav_handle = heap_->GetGPUDescriptorHandleForHeapStart();
    cmd->SetComputeRootDescriptorTable(1, uav_handle);
    D3D12_GPU_DESCRIPTOR_HANDLE srv_handle = uav_handle;
    srv_handle.ptr += static_cast<UINT64>(kUavCount) * descriptor_size_;
    cmd->SetComputeRootDescriptorTable(2, srv_handle);

    // ── Pass 0: Hit (GPU 一次交差、 シーンアップロード済みの時のみ) ──
    if ((scene_flags_ & kSharcSceneMesh) != 0) {
        cmd->SetPipelineState(pso_hit_.Get());
        cmd->Dispatch((ray_count_ + 63u) / 64u, 1, 1);
        uav_barrier_(cmd, rays_.resource.Get());
        uav_barrier_(cmd, shade_.resource.Get());
    }

    // ── Pass 1: March ──
    cmd->SetPipelineState(pso_march_.Get());
    cmd->Dispatch((ray_count_ + 63u) / 64u, 1, 1);
    uav_barrier_(cmd, counters_.resource.Get());
    uav_barrier_(cmd, keys_.resource.Get());

    // ── Pass 2: Compact ──
    cmd->SetPipelineState(pso_compact_.Get());
    cmd->Dispatch((config_.table_size + 255u) / 256u, 1, 1);
    uav_barrier_(cmd, requests_.resource.Get());
    transition_(cmd, indirect_, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);

    // ── Pass 3: Light / Update (indirect) ──
    cmd->SetPipelineState(pso_update_.Get());
    cmd->ExecuteIndirect(dispatch_indirect_sig_.Get(), 1, indirect_.resource.Get(), 0,
                         nullptr, 0);
    transition_(cmd, indirect_, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    uav_barrier_(cmd, cells_.resource.Get());

    // ── Pass 4: Resolve ──
    cmd->SetPipelineState(pso_resolve_.Get());
    cmd->Dispatch((ray_count_ + 63u) / 64u, 1, 1);
    uav_barrier_(cmd, output_.resource.Get());

    // request_count 読み出し用に counters を READBACK へコピー
    transition_(cmd, counters_, D3D12_RESOURCE_STATE_COPY_SOURCE);
    cmd->CopyBufferRegion(counters_rb_.resource.Get(), 0, counters_.resource.Get(), 0,
                          counters_rb_.size);
    transition_(cmd, counters_, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
}

void SharcDx12Executor::execute_and_wait(ID3D12GraphicsCommandList* cmd,
                                         ID3D12CommandAllocator* allocator) {
    cmd->Close();
    ID3D12CommandList* lists[] = {cmd};
    queue_->ExecuteCommandLists(1, lists);
    ++fence_value_;
    queue_->Signal(fence_.Get(), fence_value_);
    if (fence_->GetCompletedValue() < fence_value_) {
        fence_->SetEventOnCompletion(fence_value_, fence_event_);
        WaitForSingleObject(fence_event_, INFINITE);
    }
    allocator->Reset();
    cmd->Reset(allocator, nullptr);
}

void SharcDx12Executor::copy_output_to_readback(ID3D12GraphicsCommandList* cmd) {
    transition_(cmd, output_, D3D12_RESOURCE_STATE_COPY_SOURCE);
    cmd->CopyBufferRegion(output_rb_.resource.Get(), 0, output_.resource.Get(), 0,
                          output_rb_.size);
    transition_(cmd, output_, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
}

const void* SharcDx12Executor::map_readback() {
    D3D12_RANGE range{0, static_cast<SIZE_T>(output_rb_.size)};
    void* ptr = nullptr;
    if (FAILED(output_rb_.resource->Map(0, &range, &ptr))) return nullptr;
    return ptr;
}

void SharcDx12Executor::unmap_readback() {
    D3D12_RANGE empty{0, 0};
    output_rb_.resource->Unmap(0, &empty);
}

#endif // _WIN32

void SharcDx12Executor::shutdown() {
#ifdef _WIN32
    if (device_) {
        if (queue_ && fence_) {
            ++fence_value_;
            queue_->Signal(fence_.Get(), fence_value_);
            if (fence_->GetCompletedValue() < fence_value_ && fence_event_) {
                fence_->SetEventOnCompletion(fence_value_, fence_event_);
                WaitForSingleObject(fence_event_, INFINITE);
            }
        }
        if (counters_rb_.mapped) {
            D3D12_RANGE empty{0, 0};
            counters_rb_.resource->Unmap(0, &empty);
            counters_rb_.mapped = nullptr;
        }
    }
    if (fence_event_) {
        CloseHandle(fence_event_);
        fence_event_ = nullptr;
    }
    temp_uploads_.clear();
    device_.Reset();
    factory_.Reset();
#endif
    initialized_ = false;
}

} // namespace pictor
