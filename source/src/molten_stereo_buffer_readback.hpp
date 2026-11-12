#pragma once

#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <array>
#include <cstdio>
#include <cstdint>
#include <utility>
#include "compute_state.hpp"

namespace runtime_study {

// Fixture path that reads one linear buffer in place of two staging textures.
// The caller serializes the immediate context and owns the completion fence.
// Neither the stream nor the shared-memory format depends on this packing.
class MoltenStereoBufferReadback {
    template<class T> using ComPtr = Microsoft::WRL::ComPtr<T>;

    static constexpr char packSource[] = R"hlsl(
Texture2D<float4> Left : register(t0);
Texture2D<float4> Right : register(t1);
RWByteAddressBuffer Output : register(u0);

cbuffer Config : register(b0) {
    uint EyeWidth;
    uint EyeHeight;
    uint SrgbMask;
    uint OpaqueMask;
};

float encodeSrgb(float value) {
    return value <= 0.0031308f ? 12.92f * value
        : 1.055f * pow(value, 1.0f / 2.4f) - 0.055f;
}

[numthreads(16, 8, 1)]
void main(uint3 coordinate : SV_DispatchThreadID) {
    if (coordinate.x >= EyeWidth * 2 || coordinate.y >= EyeHeight) return;

    uint eyeBit = 1;
    float4 color;
    if (coordinate.x < EyeWidth) {
        color = Left.Load(int3(coordinate.xy, 0));
    } else {
        eyeBit = 2;
        color = Right.Load(int3(coordinate.x - EyeWidth, coordinate.y, 0));
    }
    // The typed sRGB SRV returns linear RGB. Convert it back to the exact
    // 8-bit wire encoding, while alpha is always linear in both formats.
    if (SrgbMask & eyeBit) {
        color.rgb = float3(encodeSrgb(color.r), encodeSrgb(color.g), encodeSrgb(color.b));
    }
    if (OpaqueMask & eyeBit) color.a = 1.0f;
    uint4 bytes = uint4(saturate(color) * 255.0f + 0.5f);
    uint pixel = bytes.b | (bytes.g << 8) | (bytes.r << 16) | (bytes.a << 24);
    Output.Store((coordinate.y * EyeWidth * 2 + coordinate.x) * 4, pixel);
}
)hlsl";

    struct Config {
        UINT width, height, srgbMask, opaqueMask;
    };

    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11ComputeShader> shader_;

public:
    struct Slot {
        ComPtr<ID3D11Buffer> output, readback, config;
        ComPtr<ID3D11UnorderedAccessView> outputView;
        std::array<ComPtr<ID3D11ShaderResourceView>, 2> inputViews;
        std::array<ComPtr<ID3D11Texture2D>, 2> samplingTextures;
        std::array<ID3D11Texture2D*, 2> inputs{};
        UINT bytes{};
    };

    [[nodiscard]] bool initialize(ID3D11Device* device) {
        if (!device || device_) return false;
        ComPtr<ID3DBlob> shader, errors;
        auto shaderResult = D3DCompile(packSource, sizeof packSource - 1, "molten_stereo_buffer_pack", nullptr,
                                       nullptr, "main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &shader, &errors);
        if (SUCCEEDED(shaderResult))
            shaderResult = device->CreateComputeShader(shader->GetBufferPointer(), shader->GetBufferSize(),
                                                        nullptr, &shader_);
        if (FAILED(shaderResult)) {
            std::fprintf(stderr, "Linear stereo shader unavailable: 0x%08lx\n", static_cast<unsigned long>(shaderResult));
            if (errors) std::fprintf(stderr, "%.240s\n", static_cast<const char*>(errors->GetBufferPointer()));
            return false;
        }
        device_ = device;
        return true;
    }

    [[nodiscard]] bool queue(ID3D11DeviceContext* context, Slot& slot,
                             const std::array<ID3D11Texture2D*, 2>& eyes, UINT width, UINT height) {
        if (!device_ || !context || !width || width > 4096 || !height || height > 4096)
            return false;
        Config config{width, height, 0, 0};
        std::array<D3D11_TEXTURE2D_DESC, 2> descriptions{};
        for (UINT eye = 0; eye < eyes.size(); ++eye) {
            if (!eyes[eye]) return false;
            auto& desc = descriptions[eye];
            eyes[eye]->GetDesc(&desc);
            if (desc.Width != width || desc.Height != height || desc.MipLevels != 1 ||
                desc.ArraySize != 1 || desc.SampleDesc.Count != 1) return false;
            const bool rgba = desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM ||
                              desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
            const bool bgra = desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM ||
                              desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
            if (!rgba && !bgra) return false;
            if (rgba) config.opaqueMask |= 1u << eye;
            if (desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ||
                desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) config.srgbMask |= 1u << eye;
        }
        if (!allocate(slot, width * height * 8)) return false;

        // D3DMetal's game context may not implement SwapDeviceContextState.
        // Keep the game's OM bindings and use a private copy for any eye that
        // is still bound for output or cannot be sampled directly.
        std::array<ID3D11RenderTargetView*, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> renderTargets{};
        context->OMGetRenderTargets(static_cast<UINT>(renderTargets.size()), renderTargets.data(), nullptr);
        std::array<bool, 2> needsCopy{};
        for (auto* target : renderTargets) if (target) {
            ComPtr<ID3D11Resource> resource;
            target->GetResource(&resource);
            for (UINT eye = 0; eye < eyes.size(); ++eye)
                needsCopy[eye] |= resource.Get() == eyes[eye];
            target->Release();
        }
        std::array<ID3D11ShaderResourceView*, 2> views{};
        for (UINT eye = 0; eye < eyes.size(); ++eye) {
            auto desc = descriptions[eye];
            needsCopy[eye] |= !(desc.BindFlags & D3D11_BIND_SHADER_RESOURCE) ||
                (desc.BindFlags & (D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_DEPTH_STENCIL));
            auto* sampled = eyes[eye];
            if (needsCopy[eye]) {
                D3D11_TEXTURE2D_DESC existing{};
                if (slot.samplingTextures[eye]) slot.samplingTextures[eye]->GetDesc(&existing);
                if (existing.Width != width || existing.Height != height || existing.Format != desc.Format) {
                    desc.Usage = D3D11_USAGE_DEFAULT;
                    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                    desc.MiscFlags = desc.CPUAccessFlags = 0;
                    ComPtr<ID3D11Texture2D> copy;
                    if (FAILED(device_->CreateTexture2D(&desc, nullptr, &copy))) return false;
                    slot.samplingTextures[eye] = std::move(copy);
                }
                sampled = slot.samplingTextures[eye].Get();
            }
            if (slot.inputs[eye] != sampled) {
                ComPtr<ID3D11ShaderResourceView> view;
                if (FAILED(device_->CreateShaderResourceView(sampled, nullptr, &view))) return false;
                slot.inputs[eye] = sampled;
                slot.inputViews[eye] = std::move(view);
            }
            views[eye] = slot.inputViews[eye].Get();
        }

        PreparationState saved(context);
        ComPtr<ID3D11Predicate> predicate;
        BOOL predicateValue{};
        context->GetPredication(&predicate,&predicateValue);
        context->SetPredication(nullptr,FALSE);
        for (UINT eye = 0; eye < eyes.size(); ++eye)
            if (needsCopy[eye]) context->CopyResource(slot.samplingTextures[eye].Get(), eyes[eye]);
        context->UpdateSubresource(slot.config.Get(), 0, nullptr, &config, 0, 0);
        auto* constants = slot.config.Get();
        auto* outputView = slot.outputView.Get();
        context->CSSetShader(shader_.Get(), nullptr, 0);
        context->CSSetConstantBuffers(0, 1, &constants);
        context->CSSetShaderResources(0, static_cast<UINT>(views.size()), views.data());
        context->CSSetUnorderedAccessViews(0, 1, &outputView, nullptr);
        context->Dispatch((width * 2 + 15) / 16, (height + 7) / 8, 1);
        ID3D11UnorderedAccessView* unbound{};
        context->CSSetUnorderedAccessViews(0, 1, &unbound, nullptr);
        const std::array<ID3D11ShaderResourceView*, 2> unboundInputs{};
        context->CSSetShaderResources(0, static_cast<UINT>(unboundInputs.size()), unboundInputs.data());
        context->CopyResource(slot.readback.Get(), slot.output.Get());
        context->SetPredication(predicate.Get(),predicateValue);
        return true;
    }

    [[nodiscard]] static ID3D11Buffer* staging(const Slot& slot) { return slot.readback.Get(); }
    [[nodiscard]] static size_t byteSize(const Slot& slot) { return slot.bytes; }

private:
    bool allocate(Slot& slot, UINT bytes) {
        if (slot.bytes == bytes) return true;
        Slot replacement{};
        D3D11_BUFFER_DESC desc{};
        desc.ByteWidth = bytes;
        desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
        if (FAILED(device_->CreateBuffer(&desc, nullptr, &replacement.output))) return false;

        D3D11_UNORDERED_ACCESS_VIEW_DESC view{};
        view.Format = DXGI_FORMAT_R32_TYPELESS;
        view.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        view.Buffer.NumElements = bytes / 4;
        view.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
        if (FAILED(device_->CreateUnorderedAccessView(replacement.output.Get(), &view,
                                                     &replacement.outputView))) return false;

        desc.BindFlags = desc.MiscFlags = 0;
        desc.Usage = D3D11_USAGE_STAGING;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(device_->CreateBuffer(&desc, nullptr, &replacement.readback))) return false;

        desc = {};
        desc.ByteWidth = sizeof(Config);
        desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        if (FAILED(device_->CreateBuffer(&desc, nullptr, &replacement.config))) return false;
        replacement.bytes = bytes;
        slot = std::move(replacement);
        return true;
    }
};

} // namespace runtime_study
