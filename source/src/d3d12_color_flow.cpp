#include "d3d12_color_flow.hpp"

#include "amd/amd_flow12.hpp"
#include "amd/camera_contract.hpp"
#include "stock_ofxr_shader.hpp"

#include <d3dcompiler.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>
#include <utility>

using Microsoft::WRL::ComPtr;

namespace runtime_study {
namespace {

using Parameters = ofxr_color_d11::Parameters;
constexpr auto shaderRead = static_cast<D3D12_RESOURCE_STATES>(
    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
constexpr unsigned descriptorsPerEye = 6;
constexpr unsigned constantsPerEye = 2;

// Same pose-aligned AMD forward warp as the managed D3D11 path. The source
// and synthetic retain the current eye pose, so OpenXR still owns head warp.
constexpr char forwardShader[] = R"HLSL(
float4 FrameGenAmdForwardPS(FullscreenVertex input) : SV_Target {
    uint2 pixel = uint2(input.position.xy) - OutputOrigin;
    float4 current = CurrentFrame.Load(int4(int2(pixel), int(Slice), 0));
    if ((FlowAuxiliary.Load(int3(1, 0, 0)) & 0x0fU) != 0) {
        return saturate(current);
    }
    float2 backward = flow_for_pixel(float2(pixel), Slice, ViewIndex, 1.0, false);
    float2 source = float2(pixel) + synthesis_fraction() * backward;
    if (!coordinate_inside_rect(source, PreviousMappings[ViewIndex].TargetRect) ||
        map_target_to_source(source, PreviousMappings[ViewIndex]).valid < 0.5) {
        return saturate(current);
    }
    return saturate(bilinear_current_source(source, Slice));
}
)HLSL";

bool sameObject(IUnknown* lhs, IUnknown* rhs) {
    if (lhs == rhs) return lhs != nullptr;
    ComPtr<IUnknown> left, right;
    return lhs && rhs &&
        SUCCEEDED(lhs->QueryInterface(IID_PPV_ARGS(&left))) &&
        SUCCEEDED(rhs->QueryInterface(IID_PPV_ARGS(&right))) && left.Get() == right.Get();
}

bool onDevice(ID3D12DeviceChild* child, ID3D12Device* device) {
    ComPtr<ID3D12Device> actual;
    return child && SUCCEEDED(child->GetDevice(IID_PPV_ARGS(&actual))) &&
        sameObject(actual.Get(), device);
}

DXGI_FORMAT colorFamily(DXGI_FORMAT format) {
    switch (format) {
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        return DXGI_FORMAT_R8G8B8A8_TYPELESS;
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
        return DXGI_FORMAT_B8G8R8A8_TYPELESS;
    default:
        return DXGI_FORMAT_UNKNOWN;
    }
}

bool compatibleColor(DXGI_FORMAT storage, DXGI_FORMAT typed) {
    const auto family = colorFamily(typed);
    return family != DXGI_FORMAT_UNKNOWN && typed != family &&
        (storage == typed || storage == family);
}

bool sameFov(const XrFovf& a, const XrFovf& b) {
    return std::abs(a.angleLeft - b.angleLeft) < 1e-6f &&
        std::abs(a.angleRight - b.angleRight) < 1e-6f &&
        std::abs(a.angleUp - b.angleUp) < 1e-6f &&
        std::abs(a.angleDown - b.angleDown) < 1e-6f;
}

ofxr_color_d11::View cameraView(const XrCompositionLayerProjectionView& view,
                               UINT width, UINT height) {
    const auto& q = view.pose.orientation;
    const auto& fov = view.fov;
    return {{q.x, q.y, q.z, q.w}, {fov.angleLeft, fov.angleRight, fov.angleUp, fov.angleDown},
            {0, 0, static_cast<float>(width), static_cast<float>(height)}};
}

bool finiteView(const XrCompositionLayerProjectionView& view) {
    if (view.type != XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW || view.next) return false;
    const auto& p = view.pose.position;
    const auto& q = view.pose.orientation;
    const float norm = q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w;
    return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z) &&
        norm > .5f && norm < 1.5f;
}

HRESULT compile(const std::string& source, const char* entry, const char* target,
                ComPtr<ID3DBlob>& code) {
    ComPtr<ID3DBlob> diagnostic;
    const auto hr = D3DCompile(source.data(), source.size(), "FrameGen-D3D12", nullptr,
        nullptr, entry, target, D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3,
        0, &code, &diagnostic);
    if (FAILED(hr) && diagnostic) {
        std::fprintf(stderr, "FrameGen DX12 %s: %.*s\n", entry,
            static_cast<int>(diagnostic->GetBufferSize()),
            static_cast<const char*>(diagnostic->GetBufferPointer()));
    }
    return hr;
}

D3D12_SHADER_BYTECODE bytecode(ID3DBlob* code) {
    return {code->GetBufferPointer(), code->GetBufferSize()};
}

void transition(ID3D12GraphicsCommandList* list, ID3D12Resource* texture,
                D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after,
                UINT subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES) {
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition = {texture, subresource, before, after};
    list->ResourceBarrier(1, &barrier);
}

D3D12_RESOURCE_DESC imageDesc(UINT width, UINT height, DXGI_FORMAT format,
                              D3D12_RESOURCE_FLAGS flags) {
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Flags = flags;
    return desc;
}

HRESULT makeResource(ID3D12Device* device, const D3D12_RESOURCE_DESC& desc,
                     D3D12_HEAP_TYPE type, D3D12_RESOURCE_STATES state,
                     ComPtr<ID3D12Resource>& result) {
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = type;
    heap.CreationNodeMask = heap.VisibleNodeMask = 1;
    return device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state,
        nullptr, IID_PPV_ARGS(&result));
}

} // namespace

struct D3D12ColorFlow::Impl {
    struct Eye {
        std::array<ComPtr<ID3D12Resource>, 2> history;
        std::array<ComPtr<ID3D12Resource>, 2> packed;
        std::unique_ptr<actual_amd::Flow12> flow;
        ComPtr<ID3D12PipelineState> draw;
        Parameters parameters{};
        DXGI_FORMAT sourceFormat{DXGI_FORMAT_UNKNOWN}, targetFormat{DXGI_FORMAT_UNKNOWN};
    };

    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> pack;
    ComPtr<ID3DBlob> vertex, fragment;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12DescriptorHeap> bindings, targets;
    ComPtr<ID3D12Resource> constants;
    std::byte* constantsData{};
    UINT descriptorStride{}, targetStride{};
    ComPtr<ID3D12Fence> fence;
    HANDLE event{};
    UINT64 nextFence{}, pendingFence{};
    bool unprovenSubmission{}, broken{};
    std::array<std::optional<Source>, 2> retainedSources;
    std::array<ComPtr<ID3D12Resource>, 2> retainedOutputs;
    std::array<Eye, 2> eyes;
    Views previous{};
    XrTime previousTime{};
    XrSpace previousSpace{XR_NULL_HANDLE};
    unsigned historyIndex{};
    std::atomic<bool> resetRequested{false};

    ~Impl() {
        if (constantsData) constants->Unmap(0, nullptr);
        if (event) CloseHandle(event);
    }

    menu_framegen::AdmissionIssue setup(ID3D12Device* d, ID3D12CommandQueue* q) {
        using menu_framegen::BlockReason;
        if (!d || !q || q->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT ||
            !onDevice(q, d)) return {BlockReason::dx12_owner, E_INVALIDARG};
        device = d;
        queue = q;
        D3D12_DESCRIPTOR_RANGE ranges[2]{};
        ranges[0] = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 4, 0, 0, 0};
        ranges[1] = {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 2, 1, 0, 0};
        D3D12_ROOT_PARAMETER parameters[3]{};
        parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        for (unsigned i = 1; i < 3; ++i) {
            parameters[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            parameters[i].DescriptorTable = {1, &ranges[i - 1]};
        }
        D3D12_ROOT_SIGNATURE_DESC rootDesc{};
        rootDesc.NumParameters = 3;
        rootDesc.pParameters = parameters;
        rootDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
        ComPtr<ID3DBlob> serialized;
        auto hr = D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1,
                &serialized, nullptr);
        if (SUCCEEDED(hr)) hr = d->CreateRootSignature(0, serialized->GetBufferPointer(),
                serialized->GetBufferSize(), IID_PPV_ARGS(&root));
        if (FAILED(hr)) return {BlockReason::dx12_root, hr};

        const std::string stock(stock_ofxr_shader);
        ComPtr<ID3DBlob> compute;
        hr = compile(stock, "PackNvidiaFlowInput", "cs_5_0", compute);
        if (SUCCEEDED(hr)) hr = compile(stock, "FullscreenTriangleVS", "vs_5_0", vertex);
        if (SUCCEEDED(hr)) hr = compile("#define XRFG_D3D12_OUTPUT_RECT\n" + stock + forwardShader,
                "FrameGenAmdForwardPS", "ps_5_0", fragment);
        if (FAILED(hr)) return {BlockReason::dx12_shader, hr};
        D3D12_COMPUTE_PIPELINE_STATE_DESC computeDesc{};
        computeDesc.pRootSignature = root.Get();
        computeDesc.CS = bytecode(compute.Get());
        hr = d->CreateComputePipelineState(&computeDesc, IID_PPV_ARGS(&pack));
        if (FAILED(hr)) return {BlockReason::dx12_pipeline, hr};
        hr = d->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator));
        if (SUCCEEDED(hr)) hr = d->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                allocator.Get(), nullptr, IID_PPV_ARGS(&list));
        if (SUCCEEDED(hr)) hr = list->Close();
        if (SUCCEEDED(hr)) hr = d->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
        if (FAILED(hr)) return {BlockReason::dx12_commands, hr};

        D3D12_DESCRIPTOR_HEAP_DESC heap{};
        heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heap.NumDescriptors = 2 * descriptorsPerEye;
        heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        hr = d->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&bindings));
        if (FAILED(hr)) return {BlockReason::dx12_descriptors, hr};
        descriptorStride = d->GetDescriptorHandleIncrementSize(heap.Type);
        heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        heap.NumDescriptors = 2;
        heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        hr = d->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&targets));
        if (FAILED(hr)) return {BlockReason::dx12_descriptors, hr};
        targetStride = d->GetDescriptorHandleIncrementSize(heap.Type);

        D3D12_RESOURCE_DESC buffer{};
        buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        buffer.Width = 2 * constantsPerEye * sizeof(Parameters);
        buffer.Height = buffer.DepthOrArraySize = buffer.MipLevels = buffer.SampleDesc.Count = 1;
        buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        D3D12_RANGE noRead{};
        hr = makeResource(d, buffer, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, constants);
        if (SUCCEEDED(hr)) hr = constants->Map(0, &noRead, reinterpret_cast<void**>(&constantsData));
        if (FAILED(hr)) return {BlockReason::dx12_constants, hr};
        event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!event) return {BlockReason::dx12_commands, HRESULT_FROM_WIN32(GetLastError())};
        return {};
    }

    bool makeEye(Eye& eye, UINT width, UINT height, DXGI_FORMAT color, DXGI_FORMAT output) {
        if (!ofxr_color_d11::set_packed_extent(eye.parameters, width, height, 1, 2)) return false;
        eye.sourceFormat = color;
        eye.targetFormat = output;
        const auto historyDesc = imageDesc(width, height, color, D3D12_RESOURCE_FLAG_NONE);
        for (auto& history : eye.history)
            if (FAILED(makeResource(device.Get(), historyDesc, D3D12_HEAP_TYPE_DEFAULT, shaderRead, history)))
                return false;
        const auto packedDesc = imageDesc(eye.parameters.packed_width, eye.parameters.packed_height,
            DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        for (auto& packed : eye.packed)
            if (FAILED(makeResource(device.Get(), packedDesc, D3D12_HEAP_TYPE_DEFAULT,
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, packed))) return false;
        eye.flow = std::make_unique<actual_amd::Flow12>();
        if (FAILED(eye.flow->initialize(device.Get(), eye.parameters.packed_width,
                eye.parameters.packed_height))) return false;
        eye.parameters.flow_width = eye.flow->width();
        eye.parameters.flow_height = eye.flow->height();

        D3D12_GRAPHICS_PIPELINE_STATE_DESC draw{};
        draw.pRootSignature = root.Get();
        draw.VS = bytecode(vertex.Get());
        draw.PS = bytecode(fragment.Get());
        auto& blend = draw.BlendState.RenderTarget[0];
        blend.SrcBlend = blend.SrcBlendAlpha = D3D12_BLEND_ONE;
        blend.DestBlend = blend.DestBlendAlpha = D3D12_BLEND_ZERO;
        blend.BlendOp = blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        blend.LogicOp = D3D12_LOGIC_OP_NOOP;
        blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        draw.SampleMask = UINT_MAX;
        draw.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        draw.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        draw.RasterizerState.DepthClipEnable = TRUE;
        draw.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        draw.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        const D3D12_DEPTH_STENCILOP_DESC noStencil{D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP,
            D3D12_STENCIL_OP_KEEP, D3D12_COMPARISON_FUNC_ALWAYS};
        draw.DepthStencilState.FrontFace = draw.DepthStencilState.BackFace = noStencil;
        draw.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        draw.NumRenderTargets = 1;
        draw.RTVFormats[0] = output;
        draw.SampleDesc.Count = 1;
        return SUCCEEDED(device->CreateGraphicsPipelineState(&draw, IID_PPV_ARGS(&eye.draw)));
    }

    D3D12_CPU_DESCRIPTOR_HANDLE cpuBinding(unsigned eye, unsigned slot) const {
        auto handle = bindings->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += static_cast<SIZE_T>(eye * descriptorsPerEye + slot) * descriptorStride;
        return handle;
    }

    D3D12_GPU_DESCRIPTOR_HANDLE gpuBinding(unsigned eye, unsigned slot) const {
        auto handle = bindings->GetGPUDescriptorHandleForHeapStart();
        handle.ptr += static_cast<UINT64>(eye * descriptorsPerEye + slot) * descriptorStride;
        return handle;
    }

    void setBindings(unsigned index, UINT current, UINT previousIndex) {
        const auto& eye = eyes[index];
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
        srv.Format = eye.sourceFormat;
        srv.Texture2DArray.ArraySize = srv.Texture2DArray.MipLevels = 1;
        device->CreateShaderResourceView(eye.history[previousIndex].Get(), &srv, cpuBinding(index, 0));
        device->CreateShaderResourceView(eye.history[current].Get(), &srv, cpuBinding(index, 1));
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Texture2D = {0, 1, 0, 0.f};
        srv.Format = DXGI_FORMAT_R16G16_SINT;
        device->CreateShaderResourceView(eye.flow->vectors(), &srv, cpuBinding(index, 2));
        srv.Format = DXGI_FORMAT_R32_UINT;
        device->CreateShaderResourceView(eye.flow->scene_change(), &srv, cpuBinding(index, 3));
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        for (unsigned i = 0; i < 2; ++i)
            device->CreateUnorderedAccessView(eye.packed[i].Get(), nullptr, &uav, cpuBinding(index, 4 + i));
    }

    D3D12_GPU_VIRTUAL_ADDRESS writeConstants(unsigned eye, unsigned slot, const Parameters& value) {
        const auto offset = (eye * constantsPerEye + slot) * sizeof(Parameters);
        std::memcpy(constantsData + offset, &value, sizeof(value));
        return constants->GetGPUVirtualAddress() + offset;
    }

    void copySource(const Source& source, const XrSwapchainSubImage& view, UINT mips,
                    ID3D12Resource* history) {
        const auto index = view.imageArrayIndex * mips;
        const auto& rect = view.imageRect;
        const D3D12_BOX box{static_cast<UINT>(rect.offset.x), static_cast<UINT>(rect.offset.y), 0,
            static_cast<UINT>(rect.offset.x + rect.extent.width),
            static_cast<UINT>(rect.offset.y + rect.extent.height), 1};
        D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
        src.pResource = source.texture.Get();
        src.Type = dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.SubresourceIndex = index;
        dst.pResource = history;
        transition(list.Get(), src.pResource, source.release_state, D3D12_RESOURCE_STATE_COPY_SOURCE, index);
        transition(list.Get(), history, shaderRead, D3D12_RESOURCE_STATE_COPY_DEST);
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
        transition(list.Get(), history, D3D12_RESOURCE_STATE_COPY_DEST, shaderRead);
        transition(list.Get(), src.pResource, D3D12_RESOURCE_STATE_COPY_SOURCE, source.release_state, index);
    }

    void recordPack(unsigned index, Parameters parameters) {
        auto& eye = eyes[index];
        parameters.synthesis_flags = 2u | 4u;
        for (const auto& packed : eye.packed)
            transition(list.Get(), packed.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        ID3D12DescriptorHeap* heap = bindings.Get();
        list->SetDescriptorHeaps(1, &heap);
        list->SetComputeRootSignature(root.Get());
        list->SetComputeRootConstantBufferView(0, writeConstants(index, 0, parameters));
        list->SetComputeRootDescriptorTable(1, gpuBinding(index, 0));
        list->SetComputeRootDescriptorTable(2, gpuBinding(index, 4));
        list->SetPipelineState(pack.Get());
        list->Dispatch((parameters.packed_width + 7) / 8, (parameters.packed_height + 7) / 8, 1);
        for (const auto& packed : eye.packed)
            transition(list.Get(), packed.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }

    void recordDraw(unsigned index, Parameters parameters, const Output& output, float phase) {
        auto& eye = eyes[index];
        parameters.synthesis_flags = 2u | ofxr_color_d11::fraction_flags(phase);
        const auto& rect = output.subImage.imageRect;
        parameters.output_origin = {static_cast<UINT>(rect.offset.x), static_cast<UINT>(rect.offset.y)};
        auto rtv = targets->GetCPUDescriptorHandleForHeapStart();
        rtv.ptr += static_cast<SIZE_T>(index) * targetStride;
        D3D12_RENDER_TARGET_VIEW_DESC target{};
        target.Format = output.format;
        target.ViewDimension = output.texture->GetDesc().DepthOrArraySize == 1 ?
            D3D12_RTV_DIMENSION_TEXTURE2D : D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
        if (target.ViewDimension == D3D12_RTV_DIMENSION_TEXTURE2DARRAY)
            target.Texture2DArray = {0, output.subImage.imageArrayIndex, 1, 0};
        device->CreateRenderTargetView(output.texture, &target, rtv);
        for (auto* flow : {eye.flow->vectors(), eye.flow->scene_change()})
            transition(list.Get(), flow, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        ID3D12DescriptorHeap* heap = bindings.Get();
        list->SetDescriptorHeaps(1, &heap);
        list->SetGraphicsRootSignature(root.Get());
        list->SetGraphicsRootConstantBufferView(0, writeConstants(index, 1, parameters));
        list->SetGraphicsRootDescriptorTable(1, gpuBinding(index, 0));
        list->SetGraphicsRootDescriptorTable(2, gpuBinding(index, 4));
        list->SetPipelineState(eye.draw.Get());
        const D3D12_VIEWPORT viewport{static_cast<float>(rect.offset.x), static_cast<float>(rect.offset.y),
            static_cast<float>(parameters.width),
            static_cast<float>(parameters.height), 0, 1};
        const D3D12_RECT scissor{rect.offset.x, rect.offset.y,
            rect.offset.x + rect.extent.width, rect.offset.y + rect.extent.height};
        list->RSSetViewports(1, &viewport);
        list->RSSetScissorRects(1, &scissor);
        list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        list->DrawInstanced(3, 1, 0, 0);
        for (auto* flow : {eye.flow->vectors(), eye.flow->scene_change()})
            transition(list.Get(), flow, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }

    void reap() noexcept {
        if (!pendingFence || unprovenSubmission) return;
        const auto completed = fence->GetCompletedValue();
        if (completed == UINT64_MAX) {
            broken = true;
            return;
        }
        if (completed < pendingFence) return;
        pendingFence = 0;
        retainedSources = {};
        retainedOutputs = {};
    }
};

D3D12ColorFlow::D3D12ColorFlow() : impl_(std::make_unique<Impl>()) {}

D3D12ColorFlow::~D3D12ColorFlow() {
    if (FAILED(drainGpu())) {
        // A stalled driver has not confirmed that its reads ended. Preserve
        // queue-owned allocations and leases rather than permit GPU use-after-free.
        (void)impl_.release();
    }
}

menu_framegen::AdmissionIssue D3D12ColorFlow::initialize(ID3D12Device* device, ID3D12CommandQueue* queue) {
    if (impl_->device) return {menu_framegen::BlockReason::generation_init, E_UNEXPECTED};
    return impl_->setup(device, queue);
}

void D3D12ColorFlow::requestReset() noexcept { impl_->resetRequested = true; }
void D3D12ColorFlow::reap() noexcept { impl_->reap(); }

HRESULT D3D12ColorFlow::drainGpu(DWORD timeoutMs) noexcept {
    auto& state = *impl_;
    state.reap();
    if (state.unprovenSubmission) {
        const auto next = ++state.nextFence;
        const auto hr = state.queue->Signal(state.fence.Get(), next);
        if (FAILED(hr)) return hr;
        state.pendingFence = next;
        state.unprovenSubmission = false;
    }
    if (!state.pendingFence) return S_OK;
    if (state.fence->GetCompletedValue() == UINT64_MAX) return DXGI_ERROR_DEVICE_REMOVED;
    const auto hr = state.fence->SetEventOnCompletion(state.pendingFence, state.event);
    if (FAILED(hr)) return hr;
    const auto wait = WaitForSingleObject(state.event, timeoutMs);
    if (wait == WAIT_TIMEOUT) return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
    if (wait != WAIT_OBJECT_0) return wait == WAIT_FAILED ? HRESULT_FROM_WIN32(GetLastError()) : E_FAIL;
    state.reap();
    return state.pendingFence ? DXGI_ERROR_DEVICE_REMOVED : S_OK;
}

D3D12ColorFlow::Result D3D12ColorFlow::generate(const std::array<Source, 2>& sources,
        const Views& views, XrTime time, XrSpace space, XrDuration forecast,
        const std::array<Output, 2>& outputs) {
    auto& state = *impl_;
    state.reap();
    if (state.broken) return Result::deviceLost;
    if (!state.list || !state.event || state.pendingFence || time <= 0 ||
        forecast <= 0 || forecast > 50'000'000)
        return Result::skipped;

    std::array<D3D12_RESOURCE_DESC, 2> descs{};
    std::array<DXGI_FORMAT, 2> formats{};
    std::array<UINT, 2> widths{}, heights{};
    bool rebuild = false;
    bool reset = !state.previousTime || time <= state.previousTime ||
        time - state.previousTime > 4 * forecast || space != state.previousSpace;
    for (unsigned i = 0; i < 2; ++i) {
        const auto& source = sources[i];
        const auto& view = views[i];
        const auto& output = outputs[i];
        if (!source.texture || !sameObject(source.queue.Get(), state.queue.Get()) ||
            source.release_state != D3D12_RESOURCE_STATE_RENDER_TARGET || !output.texture ||
            output.texture == sources[0].texture.Get() || output.texture == sources[1].texture.Get() || !finiteView(view) ||
            !onDevice(source.texture.Get(), state.device.Get()) || !onDevice(output.texture, state.device.Get()))
            return Result::skipped;
        const auto& rect = view.subImage.imageRect;
        const auto& outRect = output.subImage.imageRect;
        descs[i] = source.texture->GetDesc();
        const auto& src = descs[i];
        const auto dst = output.texture->GetDesc();
        formats[i] = static_cast<DXGI_FORMAT>(source.declared_format);
        if (rect.offset.x < 0 || rect.offset.y < 0 || rect.extent.width < 128 || rect.extent.height < 128 ||
            src.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || src.SampleDesc.Count != 1 ||
            !src.MipLevels || view.subImage.imageArrayIndex >= src.DepthOrArraySize ||
            static_cast<UINT64>(rect.offset.x) + rect.extent.width > src.Width ||
            static_cast<UINT64>(rect.offset.y) + rect.extent.height > src.Height ||
            !compatibleColor(src.Format, formats[i]) || !compatibleColor(dst.Format, output.format) ||
            dst.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || dst.SampleDesc.Count != 1 ||
            dst.MipLevels != 1 || output.subImage.imageArrayIndex >= dst.DepthOrArraySize ||
            !(dst.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) ||
            outRect.offset.x < 0 || outRect.offset.y < 0 ||
            outRect.extent.width != rect.extent.width || outRect.extent.height != rect.extent.height ||
            static_cast<UINT64>(outRect.offset.x) + outRect.extent.width > dst.Width ||
            static_cast<UINT64>(outRect.offset.y) + outRect.extent.height > dst.Height)
            return Result::skipped;
        widths[i] = static_cast<UINT>(rect.extent.width);
        heights[i] = static_cast<UINT>(rect.extent.height);

        const auto& eye = state.eyes[i];
        const auto& old = state.previous[i];
        const auto& oldRect = old.subImage.imageRect;
        rebuild |= !eye.flow || eye.parameters.width != widths[i] || eye.parameters.height != heights[i] ||
            eye.sourceFormat != formats[i] || eye.targetFormat != output.format;
        reset |= old.subImage.swapchain != view.subImage.swapchain ||
            old.subImage.imageArrayIndex != view.subImage.imageArrayIndex ||
            oldRect.offset.x != rect.offset.x || oldRect.offset.y != rect.offset.y ||
            !sameFov(old.fov, view.fov);
    }
    if (outputs[0].texture == outputs[1].texture &&
        outputs[0].subImage.imageArrayIndex == outputs[1].subImage.imageArrayIndex) {
        const auto& a = outputs[0].subImage.imageRect;
        const auto& b = outputs[1].subImage.imageRect;
        if (a.offset.x < int64_t(b.offset.x) + b.extent.width &&
            b.offset.x < int64_t(a.offset.x) + a.extent.width &&
            a.offset.y < int64_t(b.offset.y) + b.extent.height &&
            b.offset.y < int64_t(a.offset.y) + a.extent.height)
            return Result::skipped;
    }
    reset |= state.resetRequested.exchange(false) || rebuild;
    if (rebuild) {
        std::array<Impl::Eye, 2> newEyes;
        for (unsigned i = 0; i < 2; ++i) {
            if (!state.makeEye(newEyes[i], widths[i], heights[i], formats[i], outputs[i].format)) {
                state.broken = true;
                return Result::deviceLost;
            }
        }
        state.eyes = std::move(newEyes);
    }

    std::array<Parameters, 2> parameters;
    const unsigned currentIndex = reset ? 0 : 1 - state.historyIndex;
    for (unsigned i = 0; i < 2; ++i) {
        parameters[i] = state.eyes[i].parameters;
        const auto old = cameraView(reset ? views[i] : state.previous[i], widths[i], heights[i]);
        const auto current = cameraView(views[i], widths[i], heights[i]);
        if (!ofxr_color_d11::make_mapping(old, current, parameters[i].previous_mappings[0])) {
            requestReset();
            return Result::skipped;
        }
        parameters[i].previous_mappings[1] = parameters[i].previous_mappings[0];
    }

    const auto disable = [&]() {
        state.broken = true;
        return Result::deviceLost;
    };
    if (FAILED(state.allocator->Reset()) || FAILED(state.list->Reset(state.allocator.Get(), nullptr)))
        return disable();
    const float phase = reset ? 0.f : std::min(static_cast<float>(forecast) /
        static_cast<float>(time - state.previousTime), .5f);
    for (unsigned i = 0; i < 2; ++i) {
        auto& eye = state.eyes[i];
        state.copySource(sources[i], views[i].subImage, descs[i].MipLevels, eye.history[currentIndex].Get());
        state.setBindings(i, currentIndex, reset ? currentIndex : state.historyIndex);
        state.recordPack(i, parameters[i]);
        if ((!reset && FAILED(eye.flow->prepare_history(state.list.Get(), eye.packed[0].Get(), false))) ||
            FAILED(eye.flow->record(state.list.Get(), eye.packed[1].Get(), reset))) {
            state.list->Close();
            return disable();
        }
        if (!reset) state.recordDraw(i, parameters[i], outputs[i], phase);
    }
    if (FAILED(state.list->Close())) return disable();

    for (unsigned i = 0; i < 2; ++i) {
        state.retainedSources[i] = sources[i];
        if (!reset) state.retainedOutputs[i] = outputs[i].texture;
    }
    ID3D12CommandList* commandList = state.list.Get();
    state.queue->ExecuteCommandLists(1, &commandList);
    const auto marker = ++state.nextFence;
    if (FAILED(state.queue->Signal(state.fence.Get(), marker))) {
        state.unprovenSubmission = true;
        return disable();
    }
    state.pendingFence = marker;
    state.historyIndex = currentIndex;
    state.previous = views;
    state.previousTime = time;
    state.previousSpace = space;
    return reset ? Result::warmup : Result::generated;
}

} // namespace runtime_study
