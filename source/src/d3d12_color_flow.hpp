#pragma once

#include "xrfg/private_openxr_runtime.hpp"
#include "menu_framegen_profile.hpp"

#include <array>
#include <memory>

namespace runtime_study {

class D3D12ColorFlow {
public:
    using Source = xrfg::PrivateOpenXrRuntime::D3D12SourceImage;
    using Views = std::array<XrCompositionLayerProjectionView, 2>;
    struct Output {
        ID3D12Resource* texture{};
        DXGI_FORMAT format{DXGI_FORMAT_UNKNOWN};
        XrSwapchainSubImage subImage{};
    };
    enum class Result { skipped, warmup, generated, deviceLost };

    D3D12ColorFlow();
    ~D3D12ColorFlow();
    menu_framegen::AdmissionIssue initialize(ID3D12Device*, ID3D12CommandQueue*);
    // Call from the session's source worker. Source rendering, synthesis and
    // OpenXR copies are ordered by the same binding queue. The two output
    // images must be acquired and waited in RENDER_TARGET state.
    Result generate(const std::array<Source, 2>&, const Views&, XrTime, XrSpace,
                    XrDuration forecast, const std::array<Output, 2>&);
    void requestReset() noexcept;
    // Retire completed work on the source worker, or after it has stopped.
    void reap() noexcept;
    // Call once the worker has stopped, before ending/restarting the session.
    // The worker does not wait on the GPU while generating frames.
    HRESULT drainGpu(DWORD timeoutMs = 2000) noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace runtime_study
