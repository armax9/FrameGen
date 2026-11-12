#pragma once

#include <d3d12.h>
#include <memory>

namespace actual_amd {

// Native FidelityFX Optical Flow for one eye. Record calls share the SDK's
// descriptor ring. The caller must finish GPU work before reuse or teardown.
class Flow12 final {
public:
    Flow12() noexcept;
    ~Flow12();
    Flow12(Flow12&&) noexcept;
    Flow12& operator=(Flow12&&) noexcept;
    Flow12(const Flow12&) = delete;
    Flow12& operator=(const Flow12&) = delete;

    HRESULT initialize(ID3D12Device* device, UINT width, UINT height) noexcept;

    // color is an SDR RGBA8 texture in NON_PIXEL_SHADER_RESOURCE state.
    // Both outputs return to UNORDERED_ACCESS state after each dispatch.
    HRESULT record(ID3D12GraphicsCommandList* commands, ID3D12Resource* color,
                   bool reset) noexcept;
    HRESULT prepare_history(ID3D12GraphicsCommandList* commands, ID3D12Resource* color,
                            bool reset) noexcept;
    ID3D12Resource* vectors() const noexcept;
    ID3D12Resource* scene_change() const noexcept;
    UINT width() const noexcept;
    UINT height() const noexcept;

private:
    struct State;
    std::unique_ptr<State> state_;
    HRESULT record_impl(ID3D12GraphicsCommandList*, ID3D12Resource*, bool reset,
                        bool history_only) noexcept;
};

} // namespace actual_amd
