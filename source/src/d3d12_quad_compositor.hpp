#pragma once

#include <d3d12.h>
#include <d3dcompiler.h>
#include <openxr/openxr.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <climits>
#include <cmath>
#include <cstdint>
#include <limits>
#include <utility>

namespace runtime_study {

// Draws a single simple OpenXR quad over a copy of an opaque stereo projection.
// Both poses must use the projection space and represent the submitted time.
// The caller retains the textures and each descriptor slot until its DIRECT
// list has finished. Record one stereo quad per slot before that completion.
class D3D12QuadCompositor final {
public:
    struct Image {
        ID3D12Resource* texture{};
        DXGI_FORMAT format{DXGI_FORMAT_UNKNOWN};
        XrRect2Di rect{};
        uint32_t slice{};
    };

    struct Quad {
        Image image;
        XrPosef pose{};
        XrExtent2Df size{};
        XrEyeVisibility visibility{XR_EYE_VISIBILITY_BOTH};
        XrCompositionLayerFlags flags{};
        D3D12_RESOURCE_STATES sourceState{D3D12_RESOURCE_STATE_RENDER_TARGET};
    };

    struct Eye {
        Image image;
        XrPosef pose{};
        XrFovf fov{};
    };

    // parent is the quad space located in projection space at this frame's
    // displayTime. An invalid result has a zero quaternion and is rejected.
    [[nodiscard]] static XrPosef composePose(const XrPosef& parent, const XrPosef& local) {
        auto a = parent.orientation, b = local.orientation;
        if (!normalize(a) || !normalize(b) || !finite(parent.position) || !finite(local.position)) return {};
        const XrQuaternionf orientation{
            a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
            a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
            a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
        return {orientation, add(parent.position, rotate(a, local.position))};
    }

    HRESULT initialize(ID3D12Device* device, uint32_t inFlightSlots) {
        if (!device || !inFlightSlots || inFlightSlots > std::numeric_limits<uint32_t>::max() / 2 || device_)
            return E_INVALIDARG;

        D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0};
        D3D12_ROOT_PARAMETER parameters[2]{};
        parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        parameters[0].Constants = {0, 0, rootWords};
        parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameters[1].DescriptorTable = {1, &range};
        parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_STATIC_SAMPLER_DESC sampler{};
        sampler.Filter = D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT;
        sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.MaxAnisotropy = 1;
        sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        const D3D12_ROOT_SIGNATURE_DESC signature{2, parameters, 1, &sampler,
            D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT};
        Microsoft::WRL::ComPtr<ID3DBlob> serialized, vertex, pixel;
        Microsoft::WRL::ComPtr<ID3D12RootSignature> root;
        Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> sources, targets;
        auto result = D3D12SerializeRootSignature(&signature, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, nullptr);
        if (SUCCEEDED(result))
            result = device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
                IID_PPV_ARGS(&root));
        if (SUCCEEDED(result)) result = compile("QuadVS", "vs_5_0", vertex);
        if (SUCCEEDED(result)) result = compile("QuadPS", "ps_5_0", pixel);
        if (FAILED(result)) return result;

        D3D12_DESCRIPTOR_HEAP_DESC heap{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, inFlightSlots,
            D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
        result = device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&sources));
        if (FAILED(result)) return result;
        heap = {D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 2 * inFlightSlots, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
        result = device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&targets));
        if (FAILED(result)) return result;

        device_ = device;
        root_ = std::move(root);
        sources_ = std::move(sources);
        targets_ = std::move(targets);
        vertex_ = std::move(vertex);
        pixel_ = std::move(pixel);
        slots_ = inFlightSlots;
        sourceStride_ = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        targetStride_ = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        return S_OK;
    }

    // Prepare each output format before recording commands for it.
    HRESULT prepareTargetFormat(DXGI_FORMAT format) {
        const auto index = formatIndex(format);
        if (!device_ || index == formats.size()) return E_INVALIDARG;
        if (pipelines_[index]) return S_OK;

        D3D12_GRAPHICS_PIPELINE_STATE_DESC pipeline{};
        pipeline.pRootSignature = root_.Get();
        pipeline.VS = {vertex_->GetBufferPointer(), vertex_->GetBufferSize()};
        pipeline.PS = {pixel_->GetBufferPointer(), pixel_->GetBufferSize()};
        auto& blend = pipeline.BlendState.RenderTarget[0];
        blend.BlendEnable = TRUE;
        blend.SrcBlend = blend.SrcBlendAlpha = D3D12_BLEND_ONE;
        blend.DestBlend = blend.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
        blend.BlendOp = blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        blend.LogicOp = D3D12_LOGIC_OP_NOOP;
        blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        pipeline.SampleMask = UINT_MAX;
        pipeline.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pipeline.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pipeline.RasterizerState.DepthClipEnable = TRUE;
        pipeline.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        pipeline.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        const D3D12_DEPTH_STENCILOP_DESC stencil{D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP,
            D3D12_STENCIL_OP_KEEP, D3D12_COMPARISON_FUNC_ALWAYS};
        pipeline.DepthStencilState.FrontFace = pipeline.DepthStencilState.BackFace = stencil;
        pipeline.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pipeline.NumRenderTargets = 1;
        pipeline.RTVFormats[0] = format;
        pipeline.SampleDesc.Count = 1;
        return device_->CreateGraphicsPipelineState(&pipeline, IID_PPV_ARGS(&pipelines_[index]));
    }

    // On success, destination images remain in RENDER_TARGET and the quad
    // source returns to sourceState. Graphics bindings on the list are changed.
    // Invalid input returns false before recording commands or writing a slot.
    [[nodiscard]] bool record(ID3D12GraphicsCommandList* list, uint32_t slot, const Quad& quad,
                              const std::array<Eye, 2>& eyes) const {
        constexpr XrCompositionLayerFlags knownFlags = XR_COMPOSITION_LAYER_CORRECT_CHROMATIC_ABERRATION_BIT |
            XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT |
            XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
        if (!device_ || !list || list->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT || slot >= slots_ ||
            !quad.image.texture || (quad.flags & ~knownFlags) ||
            !std::isfinite(quad.size.width) || !std::isfinite(quad.size.height) ||
            quad.size.width <= 0 || quad.size.height <= 0 || !finite(quad.pose.position) ||
            (quad.visibility != XR_EYE_VISIBILITY_BOTH && quad.visibility != XR_EYE_VISIBILITY_LEFT &&
                quad.visibility != XR_EYE_VISIBILITY_RIGHT))
            return false;

        const auto sourceDesc = quad.image.texture->GetDesc();
        if (!validImage(quad.image, sourceDesc) || (sourceDesc.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE))
            return false;
        auto orientation = quad.pose.orientation;
        if (!normalize(orientation)) return false;

        std::array<Draw, 2> draws{};
        const auto normal = rotate(orientation, {0, 0, 1});
        const auto& sourceRect = quad.image.rect;
        const std::array<float, 4> rect{float(sourceRect.offset.x), float(sourceRect.offset.y),
            float(sourceRect.extent.width), float(sourceRect.extent.height)};
        const std::array<float, 2> inverseSize{1.f / float(sourceDesc.Width), 1.f / float(sourceDesc.Height)};
        const auto alphaMode = (quad.flags & XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT)
            ? ((quad.flags & XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT) ? 2u : 1u) : 0u;
        for (unsigned i = 0; i < eyes.size(); ++i) {
            const auto& eye = eyes[i];
            auto& draw = draws[i];
            if (quad.visibility == (i ? XR_EYE_VISIBILITY_LEFT : XR_EYE_VISIBILITY_RIGHT)) continue;
            if (!finite(eye.pose.position)) return false;
            if (dot(subtract(eye.pose.position, quad.pose.position), normal) <= 0) continue;
            if (!eye.image.texture || eye.image.texture == quad.image.texture) return false;
            const auto outputDesc = eye.image.texture->GetDesc();
            const auto index = formatIndex(eye.image.format);
            if (!validImage(eye.image, outputDesc) || !(outputDesc.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) ||
                index == formats.size() || !pipelines_[index] ||
                !projectCorners(quad, orientation, eye, draw.constants.corners))
                return false;
            draw.constants.sourceRect = rect;
            draw.constants.inverseSourceSize = inverseSize;
            draw.constants.alphaMode = alphaMode;
            draw.pipeline = pipelines_[index].Get();
            draw.array = outputDesc.DepthOrArraySize > 1;
        }
        if (!draws[0].pipeline && !draws[1].pipeline) return true;
        if (draws[0].pipeline && draws[1].pipeline && eyes[0].image.texture == eyes[1].image.texture &&
            eyes[0].image.slice == eyes[1].image.slice && overlaps(eyes[0].image.rect, eyes[1].image.rect))
            return false;

        auto source = sources_->GetCPUDescriptorHandleForHeapStart();
        source.ptr += SIZE_T(slot) * sourceStride_;
        auto sourceGpu = sources_->GetGPUDescriptorHandleForHeapStart();
        sourceGpu.ptr += UINT64(slot) * sourceStride_;
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = quad.image.format;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2DArray.ArraySize = srv.Texture2DArray.MipLevels = 1;
        srv.Texture2DArray.FirstArraySlice = quad.image.slice;
        device_->CreateShaderResourceView(quad.image.texture, &srv, source);

        const auto sourceSubresource = quad.image.slice * sourceDesc.MipLevels;
        transition(list, quad.image.texture, sourceSubresource, quad.sourceState, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        ID3D12DescriptorHeap* heaps[] = {sources_.Get()};
        list->SetDescriptorHeaps(1, heaps);
        list->SetGraphicsRootSignature(root_.Get());
        list->SetGraphicsRootDescriptorTable(1, sourceGpu);
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        for (unsigned i = 0; i < eyes.size(); ++i) {
            const auto& draw = draws[i];
            if (!draw.pipeline) continue;
            const auto& image = eyes[i].image;
            const auto& r = image.rect;
            auto target = targets_->GetCPUDescriptorHandleForHeapStart();
            target.ptr += (SIZE_T(slot) * 2 + i) * targetStride_;
            D3D12_RENDER_TARGET_VIEW_DESC rtv{};
            rtv.Format = image.format;
            rtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
            if (draw.array) {
                rtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
                rtv.Texture2DArray.ArraySize = 1;
                rtv.Texture2DArray.FirstArraySlice = image.slice;
            }
            device_->CreateRenderTargetView(image.texture, &rtv, target);

            const D3D12_VIEWPORT viewport{float(r.offset.x), float(r.offset.y), float(r.extent.width),
                float(r.extent.height), 0, 1};
            const D3D12_RECT scissor{r.offset.x, r.offset.y, r.offset.x + r.extent.width, r.offset.y + r.extent.height};
            list->SetPipelineState(draw.pipeline);
            list->SetGraphicsRoot32BitConstants(0, rootWords, &draw.constants, 0);
            list->RSSetViewports(1, &viewport);
            list->RSSetScissorRects(1, &scissor);
            list->OMSetRenderTargets(1, &target, FALSE, nullptr);
            list->DrawInstanced(4, 1, 0, 0);
        }
        transition(list, quad.image.texture, sourceSubresource, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, quad.sourceState);
        return true;
    }

private:
    using Float4 = std::array<float, 4>;
    struct Constants {
        std::array<Float4, 4> corners{};
        Float4 sourceRect{};
        std::array<float, 2> inverseSourceSize{};
        uint32_t alphaMode{}, padding{};
    };
    struct Draw {
        Constants constants;
        ID3D12PipelineState* pipeline{};
        bool array{};
    };
    static constexpr unsigned rootWords = 24;
    static_assert(sizeof(Constants) == rootWords * sizeof(uint32_t));
    static constexpr std::array formats{DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
        DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB};

    static size_t formatIndex(DXGI_FORMAT format) {
        return std::find(formats.begin(), formats.end(), format) - formats.begin();
    }

    static bool validImage(const Image& image, const D3D12_RESOURCE_DESC& desc) {
        const auto index = formatIndex(image.format);
        if (index == formats.size() || desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
            desc.SampleDesc.Count != 1 || !desc.MipLevels || image.slice >= desc.DepthOrArraySize)
            return false;
        const auto typeless = index < 2 ? DXGI_FORMAT_R8G8B8A8_TYPELESS : DXGI_FORMAT_B8G8R8A8_TYPELESS;
        if (desc.Format != image.format && desc.Format != typeless) return false;
        const auto& r = image.rect;
        const auto right = int64_t(r.offset.x) + r.extent.width;
        const auto bottom = int64_t(r.offset.y) + r.extent.height;
        return r.offset.x >= 0 && r.offset.y >= 0 && r.extent.width > 0 && r.extent.height > 0 &&
            uint64_t(right) <= desc.Width && uint64_t(bottom) <= desc.Height &&
            right <= D3D12_VIEWPORT_BOUNDS_MAX && bottom <= D3D12_VIEWPORT_BOUNDS_MAX;
    }

    static bool overlaps(const XrRect2Di& a, const XrRect2Di& b) {
        return a.offset.x < b.offset.x + b.extent.width && b.offset.x < a.offset.x + a.extent.width &&
            a.offset.y < b.offset.y + b.extent.height && b.offset.y < a.offset.y + a.extent.height;
    }

    static bool finite(const XrVector3f& p) {
        return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z);
    }

    static bool normalize(XrQuaternionf& q) {
        const float length = q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w;
        if (!std::isfinite(length) || length < 0.5f || length > 1.5f) return false;
        const float scale = 1.f / std::sqrt(length);
        q = {q.x * scale, q.y * scale, q.z * scale, q.w * scale};
        return true;
    }

    static XrVector3f add(const XrVector3f& a, const XrVector3f& b) {
        return {a.x + b.x, a.y + b.y, a.z + b.z};
    }

    static XrVector3f subtract(const XrVector3f& a, const XrVector3f& b) {
        return {a.x - b.x, a.y - b.y, a.z - b.z};
    }

    static XrVector3f cross(const XrVector3f& a, const XrVector3f& b) {
        return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
    }

    static float dot(const XrVector3f& a, const XrVector3f& b) {
        return a.x * b.x + a.y * b.y + a.z * b.z;
    }

    static XrVector3f rotate(const XrQuaternionf& q, const XrVector3f& p) {
        const XrVector3f vector{q.x, q.y, q.z};
        const auto t = cross(vector, p);
        const auto u = cross(vector, t);
        return {p.x + 2 * (q.w * t.x + u.x), p.y + 2 * (q.w * t.y + u.y),
            p.z + 2 * (q.w * t.z + u.z)};
    }

    static bool projectCorners(const Quad& quad, const XrQuaternionf& quadOrientation,
                               const Eye& eye, std::array<Float4, 4>& corners) {
        auto inverse = eye.pose.orientation;
        if (!normalize(inverse)) return false;
        inverse.x = -inverse.x;
        inverse.y = -inverse.y;
        inverse.z = -inverse.z;

        const auto& fov = eye.fov;
        constexpr float halfPi = 1.57079632679f;
        for (const float angle : {fov.angleLeft, fov.angleRight, fov.angleDown, fov.angleUp})
            if (!std::isfinite(angle) || std::abs(angle) >= halfPi) return false;
        const float left = std::tan(fov.angleLeft), right = std::tan(fov.angleRight);
        const float down = std::tan(fov.angleDown), up = std::tan(fov.angleUp);
        // Signed spans also support OpenXR's deliberately flipped FOV axes.
        if (std::abs(right - left) < 1e-6f || std::abs(up - down) < 1e-6f) return false;

        for (unsigned i = 0; i < corners.size(); ++i) {
            const XrVector3f local{(i & 1 ? .5f : -.5f) * quad.size.width,
                (i & 2 ? -.5f : .5f) * quad.size.height, 0};
            const auto world = add(quad.pose.position, rotate(quadOrientation, local));
            const auto view = rotate(inverse, subtract(world, eye.pose.position));
            const float depth = -view.z;
            constexpr float nearDistance = .001f;
            auto& clip = corners[i];
            clip = {(2 * view.x - (right + left) * depth) / (right - left),
                (2 * view.y - (up + down) * depth) / (up - down), depth - nearDistance, depth};
            for (const auto value : clip)
                if (!std::isfinite(value)) return false;
        }
        return true;
    }

    static void transition(ID3D12GraphicsCommandList* list, ID3D12Resource* image, UINT subresource,
                           D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
        if (before == after) return;
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition = {image, subresource, before, after};
        list->ResourceBarrier(1, &barrier);
    }

    static HRESULT compile(const char* entry, const char* target, Microsoft::WRL::ComPtr<ID3DBlob>& output) {
        return D3DCompile(shader, sizeof(shader) - 1, "FrameGen OpenXR quad", nullptr, nullptr, entry, target,
            D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &output, nullptr);
    }

    static constexpr char shader[] = R"HLSL(
cbuffer QuadConstants : register(b0) {
    float4 Corners[4];
    float4 SourceRect;
    float2 InverseSourceSize;
    uint AlphaMode;
    uint Padding;
};
Texture2DArray<float4> Source : register(t0);
SamplerState LinearSampler : register(s0);

struct Vertex {
    float4 position : SV_Position;
    float2 sourcePixel : TEXCOORD0;
};

Vertex QuadVS(uint id : SV_VertexID) {
    Vertex result;
    result.position = Corners[id];
    result.sourcePixel = SourceRect.xy + SourceRect.zw * float2(id & 1u, id >> 1u);
    return result;
}

float4 PremultipliedTexel(int2 pixel) {
    float4 texel = Source.Load(int4(pixel, 0, 0));
    texel.rgb *= texel.a;
    return texel;
}

float4 QuadPS(Vertex input) : SV_Target {
    float2 pixel = clamp(input.sourcePixel, SourceRect.xy + 0.5, SourceRect.xy + SourceRect.zw - 0.5);
    if (AlphaMode == 2u) {
        // Filter already-premultiplied values to avoid fringes at clear texels.
        float2 position = pixel - 0.5;
        int2 low = int2(floor(position));
        int2 high = min(low + 1, int2(SourceRect.xy + SourceRect.zw) - 1);
        float2 weight = frac(position);
        float4 top = lerp(PremultipliedTexel(low), PremultipliedTexel(int2(high.x, low.y)), weight.x);
        float4 bottom = lerp(PremultipliedTexel(int2(low.x, high.y)), PremultipliedTexel(high), weight.x);
        return saturate(lerp(top, bottom, weight.y));
    }
    float4 color = Source.SampleLevel(LinearSampler, float3(pixel * InverseSourceSize, 0), 0);
    if (AlphaMode == 0u) color.a = 1.0;
    return saturate(color);
}
)HLSL";

    Microsoft::WRL::ComPtr<ID3D12Device> device_;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root_;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> sources_, targets_;
    Microsoft::WRL::ComPtr<ID3DBlob> vertex_, pixel_;
    std::array<Microsoft::WRL::ComPtr<ID3D12PipelineState>, formats.size()> pipelines_;
    uint32_t slots_{}, sourceStride_{}, targetStride_{};
};

} // namespace runtime_study
