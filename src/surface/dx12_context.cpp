#include "pictor/surface/dx12_context.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <algorithm>
#include <climits>
#include <cstring>
#include <vector>

namespace pictor {
using Microsoft::WRL::ComPtr;

struct Dx12Context::State {
    ComPtr<IDXGIFactory6> factory;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<IDXGISwapChain3> swapchain;
    ComPtr<ID3D12DescriptorHeap> rtv_heap;
    std::vector<ComPtr<ID3D12Resource>> buffers;
    std::vector<ComPtr<ID3D12CommandAllocator>> allocators;
    ComPtr<ID3D12GraphicsCommandList> command_list;
    ComPtr<ID3D12RootSignature> root_signature;
    ComPtr<ID3D12PipelineState> pipeline;
    ComPtr<ID3D12Resource> vertex_buffer;
    ComPtr<ID3D12Resource> index_buffer;
    D3D12_VERTEX_BUFFER_VIEW vertex_view{};
    D3D12_INDEX_BUFFER_VIEW index_view{};
    uint32_t vertex_count = 0;
    uint32_t index_count = 0;
    ComPtr<ID3D12Fence> fence;
    HANDLE fence_event = nullptr;
    UINT rtv_stride = 0;
    UINT frame_index = 0;
    uint64_t fence_value = 0;

    ~State() { if (fence_event) CloseHandle(fence_event); }
};

namespace {
bool wait_idle(Dx12Context::State& state) {
    const uint64_t target = ++state.fence_value;
    if (FAILED(state.queue->Signal(state.fence.Get(), target))) return false;
    if (state.fence->GetCompletedValue() >= target) return true;
    if (FAILED(state.fence->SetEventOnCompletion(target, state.fence_event))) return false;
    return WaitForSingleObject(state.fence_event, 5000) == WAIT_OBJECT_0;
}

Dx12Context::State* create_state(void* window, const NativeRenderConfig& config) {
    if (!window || config.width == 0 || config.height == 0 || config.image_count < 2) return nullptr;
    auto* state = new Dx12Context::State();
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&state->factory)))) { delete state; return nullptr; }

    ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0; state->factory->EnumAdapterByGpuPreference(
             i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter)) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 desc{};
        adapter->GetDesc1(&desc);
        if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0 &&
            SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0,
                                        IID_PPV_ARGS(&state->device)))) break;
        adapter.Reset();
    }
    if (!state->device) { delete state; return nullptr; }

    D3D12_COMMAND_QUEUE_DESC queue_desc{};
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(state->device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&state->queue)))) {
        delete state; return nullptr;
    }
    DXGI_SWAP_CHAIN_DESC1 swap_desc{};
    swap_desc.Width = config.width;
    swap_desc.Height = config.height;
    swap_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    swap_desc.SampleDesc.Count = 1;
    swap_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swap_desc.BufferCount = config.image_count;
    swap_desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    ComPtr<IDXGISwapChain1> swapchain1;
    if (FAILED(state->factory->CreateSwapChainForHwnd(
            state->queue.Get(), static_cast<HWND>(window), &swap_desc,
            nullptr, nullptr, &swapchain1)) ||
        FAILED(swapchain1.As(&state->swapchain))) { delete state; return nullptr; }
    state->factory->MakeWindowAssociation(static_cast<HWND>(window), DXGI_MWA_NO_ALT_ENTER);

    D3D12_DESCRIPTOR_HEAP_DESC heap_desc{};
    heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    heap_desc.NumDescriptors = config.image_count;
    if (FAILED(state->device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&state->rtv_heap)))) {
        delete state; return nullptr;
    }
    state->rtv_stride = state->device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    state->buffers.resize(config.image_count);
    state->allocators.resize(config.image_count);
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = state->rtv_heap->GetCPUDescriptorHandleForHeapStart();
    for (UINT i = 0; i < config.image_count; ++i) {
        if (FAILED(state->swapchain->GetBuffer(i, IID_PPV_ARGS(&state->buffers[i]))) ||
            FAILED(state->device->CreateCommandAllocator(
                D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&state->allocators[i])))) {
            delete state; return nullptr;
        }
        state->device->CreateRenderTargetView(state->buffers[i].Get(), nullptr, rtv);
        rtv.ptr += state->rtv_stride;
    }
    if (FAILED(state->device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
            state->allocators[0].Get(), nullptr, IID_PPV_ARGS(&state->command_list)))) {
        delete state; return nullptr;
    }
    state->command_list->Close();
    D3D12_ROOT_SIGNATURE_DESC root_desc{};
    root_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    ComPtr<ID3DBlob> root_blob;
    ComPtr<ID3DBlob> error_blob;
    if (FAILED(D3D12SerializeRootSignature(&root_desc, D3D_ROOT_SIGNATURE_VERSION_1,
                                           &root_blob, &error_blob)) ||
        FAILED(state->device->CreateRootSignature(0, root_blob->GetBufferPointer(),
              root_blob->GetBufferSize(), IID_PPV_ARGS(&state->root_signature)))) {
        delete state; return nullptr;
    }
    static constexpr char shader[] =
        "struct I{float3 p:POSITION;float4 c:COLOR;};struct V{float4 p:SV_Position;float4 c:COLOR;};"
        "V vs(I i){V o;o.p=float4(i.p,1);o.c=i.c;return o;}"
        "float4 ps(V i):SV_Target{return i.c;}";
    ComPtr<ID3DBlob> vertex;
    ComPtr<ID3DBlob> pixel;
    if (FAILED(D3DCompile(shader, sizeof(shader), nullptr, nullptr, nullptr, "vs", "vs_5_1", 0, 0,
                          &vertex, &error_blob)) ||
        FAILED(D3DCompile(shader, sizeof(shader), nullptr, nullptr, nullptr, "ps", "ps_5_1", 0, 0,
                          &pixel, &error_blob))) { delete state; return nullptr; }
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pipeline_desc{};
    pipeline_desc.pRootSignature = state->root_signature.Get();
    const D3D12_INPUT_ELEMENT_DESC input[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12,
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    };
    pipeline_desc.InputLayout = {input, 2};
    pipeline_desc.VS = {vertex->GetBufferPointer(), vertex->GetBufferSize()};
    pipeline_desc.PS = {pixel->GetBufferPointer(), pixel->GetBufferSize()};
    pipeline_desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pipeline_desc.SampleMask = UINT_MAX;
    pipeline_desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pipeline_desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pipeline_desc.DepthStencilState.DepthEnable = FALSE;
    pipeline_desc.DepthStencilState.StencilEnable = FALSE;
    pipeline_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pipeline_desc.NumRenderTargets = 1;
    pipeline_desc.RTVFormats[0] = DXGI_FORMAT_B8G8R8A8_UNORM;
    pipeline_desc.SampleDesc.Count = 1;
    if (FAILED(state->device->CreateGraphicsPipelineState(&pipeline_desc,
                                                          IID_PPV_ARGS(&state->pipeline)))) {
        delete state; return nullptr;
    }
    if (FAILED(state->device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                                          IID_PPV_ARGS(&state->fence)))) { delete state; return nullptr; }
    state->fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!state->fence_event) { delete state; return nullptr; }
    return state;
}
}

Dx12Context::Dx12Context() = default;
Dx12Context::~Dx12Context() { shutdown(); }

bool Dx12Context::initialize(void* hwnd, const NativeRenderConfig& config) {
    if (initialized_) return false;
    State* replacement = create_state(hwnd, config);
    if (!replacement) return false;
    state_ = replacement;
    hwnd_ = hwnd;
    config_ = config;
    initialized_ = true;
    return true;
}

bool Dx12Context::upload_mesh(const NativeMeshView& mesh) {
    if (!state_ || !mesh.vertices || mesh.vertex_count == 0) return false;
    const UINT64 vertex_bytes = sizeof(NativeVertex) * UINT64{mesh.vertex_count};
    const UINT64 index_bytes = sizeof(uint32_t) * UINT64{mesh.index_count};
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    auto create_upload = [&](UINT64 bytes, ComPtr<ID3D12Resource>& resource) {
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = bytes;
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        return SUCCEEDED(state_->device->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr, IID_PPV_ARGS(&resource)));
    };
    ComPtr<ID3D12Resource> vertices;
    if (!create_upload(vertex_bytes, vertices)) return false;
    void* mapped = nullptr;
    D3D12_RANGE no_read{0, 0};
    if (FAILED(vertices->Map(0, &no_read, &mapped))) return false;
    std::memcpy(mapped, mesh.vertices, static_cast<size_t>(vertex_bytes));
    vertices->Unmap(0, nullptr);
    ComPtr<ID3D12Resource> indices;
    if (mesh.index_count > 0) {
        if (!mesh.indices || !create_upload(index_bytes, indices)) return false;
        if (FAILED(indices->Map(0, &no_read, &mapped))) return false;
        std::memcpy(mapped, mesh.indices, static_cast<size_t>(index_bytes));
        indices->Unmap(0, nullptr);
    }
    state_->vertex_buffer = vertices;
    state_->index_buffer = indices;
    state_->vertex_view = {vertices->GetGPUVirtualAddress(),
                           static_cast<UINT>(vertex_bytes), sizeof(NativeVertex)};
    state_->index_view = indices ? D3D12_INDEX_BUFFER_VIEW{
        indices->GetGPUVirtualAddress(), static_cast<UINT>(index_bytes),
        DXGI_FORMAT_R32_UINT} : D3D12_INDEX_BUFFER_VIEW{};
    state_->vertex_count = mesh.vertex_count;
    state_->index_count = mesh.index_count;
    if (mesh.vertices != mesh_vertices_.data()) {
        mesh_vertices_.assign(mesh.vertices, mesh.vertices + mesh.vertex_count);
    }
    if (mesh.index_count == 0) {
        mesh_indices_.clear();
    } else if (mesh.indices != mesh_indices_.data()) {
        mesh_indices_.assign(mesh.indices, mesh.indices + mesh.index_count);
    }
    return true;
}

void Dx12Context::shutdown() {
    if (state_) wait_idle(*state_);
    delete state_;
    state_ = nullptr;
    initialized_ = false;
}

FrameResult Dx12Context::acquire_frame() {
    if (!initialized_ || !state_) return {FrameStatus::NotInitialized};
    if (!hwnd_ || !IsWindow(static_cast<HWND>(hwnd_))) return {FrameStatus::SurfaceLost};
    const HRESULT removed = state_->device->GetDeviceRemovedReason();
    if (FAILED(removed)) return {FrameStatus::DeviceLost};
    state_->frame_index = state_->swapchain->GetCurrentBackBufferIndex();
    auto& allocator = state_->allocators[state_->frame_index];
    if (FAILED(allocator->Reset()) || FAILED(state_->command_list->Reset(allocator.Get(), nullptr))) {
        return {FrameStatus::DeviceLost};
    }
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = state_->buffers[state_->frame_index].Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    state_->command_list->ResourceBarrier(1, &barrier);
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = state_->rtv_heap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += static_cast<SIZE_T>(state_->frame_index) * state_->rtv_stride;
    state_->command_list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    state_->command_list->ClearRenderTargetView(rtv, config_.clear_color, 0, nullptr);
    D3D12_VIEWPORT viewport{0.0f, 0.0f, static_cast<float>(config_.width),
                            static_cast<float>(config_.height), 0.0f, 1.0f};
    D3D12_RECT scissor{0, 0, static_cast<LONG>(config_.width), static_cast<LONG>(config_.height)};
    state_->command_list->SetGraphicsRootSignature(state_->root_signature.Get());
    state_->command_list->SetPipelineState(state_->pipeline.Get());
    state_->command_list->RSSetViewports(1, &viewport);
    state_->command_list->RSSetScissorRects(1, &scissor);
    state_->command_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    if (state_->vertex_buffer) {
        state_->command_list->IASetVertexBuffers(0, 1, &state_->vertex_view);
        if (state_->index_buffer) {
            state_->command_list->IASetIndexBuffer(&state_->index_view);
            state_->command_list->DrawIndexedInstanced(state_->index_count, 1, 0, 0, 0);
        } else {
            state_->command_list->DrawInstanced(state_->vertex_count, 1, 0, 0);
        }
    }
    if (config_.draw) {
        config_.draw(state_->command_list.Get(), state_->frame_index,
                     config_.draw_user_data);
    }
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    state_->command_list->ResourceBarrier(1, &barrier);
    return {FrameStatus::Ready, state_->frame_index, false};
}

FrameResult Dx12Context::present_frame() {
    if (!initialized_ || !state_) return {FrameStatus::NotInitialized};
    if (FAILED(state_->command_list->Close())) return {FrameStatus::DeviceLost};
    ID3D12CommandList* lists[] = {state_->command_list.Get()};
    state_->queue->ExecuteCommandLists(1, lists);
    const HRESULT result = state_->swapchain->Present(config_.vsync ? 1 : 0, 0);
    if (result == DXGI_STATUS_OCCLUDED) return {FrameStatus::RecreateSwapchain};
    if (result == DXGI_ERROR_DEVICE_REMOVED || result == DXGI_ERROR_DEVICE_RESET) {
        return {FrameStatus::DeviceLost};
    }
    if (FAILED(result)) return {FrameStatus::SurfaceLost};
    if (!wait_idle(*state_)) return {FrameStatus::DeviceLost};
    return {FrameStatus::Ready, state_->frame_index, false};
}

FrameResult Dx12Context::recover_surface(void* hwnd, const NativeRenderConfig& config) {
    if (hwnd == hwnd_ && state_ && state_->device->GetDeviceRemovedReason() == S_OK) {
        if (!wait_idle(*state_)) return {FrameStatus::DeviceLost};
        state_->command_list.Reset();
        state_->allocators.clear();
        for (auto& buffer : state_->buffers) buffer.Reset();
        // Buffer count 0 preserves the existing allocator/RTV cardinality.
        // Count changes use a new native surface/context replacement instead.
        const HRESULT resized = state_->swapchain->ResizeBuffers(
            0, config.width, config.height, DXGI_FORMAT_B8G8R8A8_UNORM, 0);
        if (FAILED(resized)) return {FrameStatus::SurfaceLost};
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = state_->rtv_heap->GetCPUDescriptorHandleForHeapStart();
        state_->allocators.resize(state_->buffers.size());
        for (UINT i = 0; i < state_->buffers.size(); ++i) {
            if (FAILED(state_->swapchain->GetBuffer(i, IID_PPV_ARGS(&state_->buffers[i])))) {
                return {FrameStatus::SurfaceLost};
            }
            if (FAILED(state_->device->CreateCommandAllocator(
                    D3D12_COMMAND_LIST_TYPE_DIRECT,
                    IID_PPV_ARGS(&state_->allocators[i])))) {
                return {FrameStatus::DeviceLost};
            }
            state_->device->CreateRenderTargetView(state_->buffers[i].Get(), nullptr, rtv);
            rtv.ptr += state_->rtv_stride;
        }
        if (FAILED(state_->device->CreateCommandList(
                0, D3D12_COMMAND_LIST_TYPE_DIRECT, state_->allocators[0].Get(),
                nullptr, IID_PPV_ARGS(&state_->command_list)))) {
            return {FrameStatus::DeviceLost};
        }
        state_->command_list->Close();
        config_ = config;
        config_.image_count = static_cast<uint32_t>(state_->buffers.size());
        return {FrameStatus::Ready};
    }
    State* replacement = create_state(hwnd, config);
    if (!replacement) return {hwnd ? FrameStatus::Error : FrameStatus::SurfaceLost};
    State* retired = state_;
    state_ = replacement;
    hwnd_ = hwnd;
    config_ = config;
    initialized_ = true;
    if (!mesh_vertices_.empty()) {
        const NativeMeshView retained{mesh_vertices_.data(),
            static_cast<uint32_t>(mesh_vertices_.size()), mesh_indices_.data(),
            static_cast<uint32_t>(mesh_indices_.size())};
        if (!upload_mesh(retained)) {
            state_ = retired;
            delete replacement;
            return {FrameStatus::Error};
        }
    }
    if (retired) wait_idle(*retired);
    delete retired;
    return {FrameStatus::Ready};
}

FrameResult Dx12Context::recover_device() {
    State* retired = state_;
    state_ = nullptr;
    initialized_ = false;
    delete retired;
    State* replacement = create_state(hwnd_, config_);
    if (!replacement) return {FrameStatus::DeviceLost};
    state_ = replacement;
    initialized_ = true;
    if (!mesh_vertices_.empty()) {
        const NativeMeshView retained{mesh_vertices_.data(),
            static_cast<uint32_t>(mesh_vertices_.size()), mesh_indices_.data(),
            static_cast<uint32_t>(mesh_indices_.size())};
        if (!upload_mesh(retained)) return {FrameStatus::DeviceLost};
    }
    return {FrameStatus::Ready};
}

} // namespace pictor
#else
namespace pictor {
Dx12Context::Dx12Context() = default;
Dx12Context::~Dx12Context() = default;
bool Dx12Context::initialize(void*, const NativeRenderConfig&) { return false; }
bool Dx12Context::upload_mesh(const NativeMeshView&) { return false; }
void Dx12Context::shutdown() {}
FrameResult Dx12Context::acquire_frame() { return {FrameStatus::NotInitialized}; }
FrameResult Dx12Context::present_frame() { return {FrameStatus::NotInitialized}; }
FrameResult Dx12Context::recover_surface(void*, const NativeRenderConfig&) { return {FrameStatus::Error}; }
FrameResult Dx12Context::recover_device() { return {FrameStatus::Error}; }
}
#endif
