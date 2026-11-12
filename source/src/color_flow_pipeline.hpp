#pragma once
#include <windows.h>
#include <d3d11_4.h>
#include <wrl/client.h>
#include <openxr/openxr.h>
#include <array>
#include <cstdint>
#include <memory>
#include <string>

namespace runtime_study {
// Caller serializes submission with the game's immediate-context encoding.
// Full D3D11 context state is swapped out when supported; otherwise the
// exact draw/compute bindings changed by this pipeline are saved/restored.
class ColorFlowPipeline {
public:
    using Texture=Microsoft::WRL::ComPtr<ID3D11Texture2D>;
    using Views=std::array<XrCompositionLayerProjectionView,2>;
    struct Result {
        enum Status { unavailable, warmup, generated } status=unavailable;
        std::array<Texture,2> textures;
        XrTime sourceTime{};
        XrSpace space=XR_NULL_HANDLE;
        bool repeated{};
    };
    ColorFlowPipeline();
    ~ColorFlowPipeline();
    ColorFlowPipeline(const ColorFlowPipeline&)=delete;
    ColorFlowPipeline& operator=(const ColorFlowPipeline&)=delete;
    bool initialize(ID3D11Device*,ID3D11DeviceContext*,const std::string& stockShader,std::string& diagnostics,
                    bool managed=false);
    // Positive forwardPhase predicts object motion from the current views.
    // Forward callers submit the same source pose, leaving head warp to the runtime.
    bool generate(const std::array<Texture,2>& source,const Views& current,XrTime sourceTime,
                  const Views& target,Result&,XrSpace space=XR_NULL_HANDLE,bool forceReset=false,
                  const std::array<int64_t,2>& declaredFormats={},int64_t forecastPeriodNs=0);
    // Queue the two source reads before the native swapchain images are
    // released. Dispatch reads only the pipeline's owned history textures.
    bool captureSource(const std::array<Texture,2>&,const Views&,XrTime,XrSpace,bool,
                       const std::array<int64_t,2>&,int64_t forecastPeriodNs);
    bool dispatchCaptured(const Views& target,Result&);
    // No CPU image readback. Signal the final ordered GPU command, then wait
    // outside the caller's encoding critical section. Failure means the caller
    // must retain this pipeline and its source leases instead of freeing them.
    HRESULT drainGpu(DWORD timeoutMs=2000) noexcept;
    void requestReset() noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
