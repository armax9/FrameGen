#include "amd_flow12.hpp"
#include "dx12_sdk_platform.hpp"

namespace framegen_amd_dx12 {
#include <FidelityFX/host/backends/dx12/ffx_dx12.h>
#include <FidelityFX/host/ffx_opticalflow.h>
#include "ffx_shader_blobs.h"
#include "embedded_opticalflow12.hpp"

extern "C" FfxErrorCode framegenDx12PrepareOpticalFlowHistory(
    FfxOpticalflowContext*, const FfxOpticalflowDispatchDescription*);

extern "C" FfxErrorCode ffxIsWave64(FfxEffect effect, uint32_t, bool& is_wave64)
{
    is_wave64 = false;
    return effect == FFX_EFFECT_OPTICALFLOW ? FFX_OK : FFX_ERROR_INVALID_ARGUMENT;
}

extern "C" FfxErrorCode ffxGetPermutationBlobByIndex(
    FfxEffect effect, FfxPass pass, FfxBindStage stage, uint32_t, FfxShaderBlob* output)
{
    if (!output) return FFX_ERROR_INVALID_POINTER;
    if (effect != FFX_EFFECT_OPTICALFLOW || stage != FFX_BIND_COMPUTE_SHADER_STAGE ||
        pass >= FFX_OPTICALFLOW_PASS_COUNT)
        return FFX_ERROR_INVALID_ARGUMENT;
    static_assert(std::size(optical_flow_shaders::blobs) == FFX_OPTICALFLOW_PASS_COUNT);
    std::memcpy(output, &optical_flow_shaders::blobs[pass], sizeof(*output));
    return FFX_OK;
}
}

namespace actual_amd {
namespace {
using Microsoft::WRL::ComPtr;
namespace sdk = framegen_amd_dx12;

HRESULT makeOutput(ID3D12Device* device, const sdk::FfxResourceDescription& source,
                   DXGI_FORMAT format, ComPtr<ID3D12Resource>& result) noexcept
{
    if (!source.width || !source.height || source.depth != 1 || source.mipCount != 1)
        return E_INVALIDARG;
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    heap.CreationNodeMask = 1;
    heap.VisibleNodeMask = 1;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = source.width;
    desc.Height = source.height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    return device->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        nullptr, IID_PPV_ARGS(result.GetAddressOf()));
}
}

struct Flow12::State {
    // The SDK carves aligned host structures and constant buffers out of scratch.
    struct alignas(256) Block { std::byte data[256]; };
    std::vector<Block> scratch;
    sdk::FfxOpticalflowContext context{};
    ComPtr<ID3D12Resource> vectors, scene_change;
    UINT input_width{}, input_height{}, flow_width{}, flow_height{};
    bool initialized{}, failed{};

    ~State()
    {
        if (initialized) sdk::ffxOpticalflowContextDestroy(&context);
    }
};

Flow12::Flow12() noexcept = default;
Flow12::~Flow12() = default;
Flow12::Flow12(Flow12&&) noexcept = default;
Flow12& Flow12::operator=(Flow12&&) noexcept = default;

HRESULT Flow12::initialize(ID3D12Device* device, UINT width, UINT height) noexcept
{
    if (!device || !width || !height || state_) return E_INVALIDARG;
    D3D12_FEATURE_DATA_SHADER_MODEL model{D3D_SHADER_MODEL_6_2};
    if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &model, sizeof model)) ||
        model.HighestShaderModel < D3D_SHADER_MODEL_6_2)
        return DXGI_ERROR_UNSUPPORTED;
    D3D12_FEATURE_DATA_D3D12_OPTIONS1 options{};
    if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS1, &options, sizeof options)) ||
        !options.WaveOps)
        return DXGI_ERROR_UNSUPPORTED;
    try {
        auto state = std::make_unique<State>();
        const auto bytes = sdk::ffxGetScratchMemorySizeDX12(FFX_OPTICALFLOW_CONTEXT_COUNT);
        if (!bytes) return E_FAIL;
        state->scratch.resize((bytes + sizeof(State::Block) - 1) / sizeof(State::Block));

        sdk::FfxOpticalflowContextDescription settings{};
        settings.resolution = {width, height};
        if (sdk::ffxGetInterfaceDX12(
                &settings.backendInterface, sdk::ffxGetDeviceDX12(device), state->scratch.data(),
                state->scratch.size() * sizeof(State::Block), FFX_OPTICALFLOW_CONTEXT_COUNT) != sdk::FFX_OK ||
            sdk::ffxOpticalflowContextCreate(&state->context, &settings) != sdk::FFX_OK)
            return E_FAIL;
        state->initialized = true;

        sdk::FfxOpticalflowSharedResourceDescriptions outputs{};
        if (sdk::ffxOpticalflowGetSharedResourceDescriptions(&state->context, &outputs) != sdk::FFX_OK)
            return E_FAIL;
        const auto& vectors = outputs.opticalFlowVector.resourceDescription;
        const auto& scene = outputs.opticalFlowSCD.resourceDescription;
        if (vectors.format != sdk::FFX_SURFACE_FORMAT_R16G16_SINT ||
            scene.format != sdk::FFX_SURFACE_FORMAT_R32_UINT || scene.width != 3 || scene.height != 1)
            return E_FAIL;

        auto result = makeOutput(device, vectors, DXGI_FORMAT_R16G16_SINT, state->vectors);
        if (FAILED(result)) return result;
        result = makeOutput(device, scene, DXGI_FORMAT_R32_UINT, state->scene_change);
        if (FAILED(result)) return result;
        state->input_width = width;
        state->input_height = height;
        state->flow_width = vectors.width;
        state->flow_height = vectors.height;
        state_ = std::move(state);
        return S_OK;
    } catch (...) {
        return E_OUTOFMEMORY;
    }
}

HRESULT Flow12::record(ID3D12GraphicsCommandList* commands, ID3D12Resource* color,
                       bool reset) noexcept
{
    return record_impl(commands, color, reset, false);
}

HRESULT Flow12::prepare_history(ID3D12GraphicsCommandList* commands, ID3D12Resource* color,
                                bool reset) noexcept
{
    return record_impl(commands, color, reset, true);
}

HRESULT Flow12::record_impl(ID3D12GraphicsCommandList* commands, ID3D12Resource* color,
                            bool reset, bool history_only) noexcept
{
    if (!state_ || state_->failed || !commands || !color) return E_INVALIDARG;
    auto& state = *state_;
    const auto desc = color->GetDesc();
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
        desc.Format != DXGI_FORMAT_R8G8B8A8_UNORM || desc.Width != state.input_width ||
        desc.Height != state.input_height || desc.DepthOrArraySize != 1 ||
        desc.MipLevels != 1 || desc.SampleDesc.Count != 1 ||
        (desc.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE))
        return E_INVALIDARG;

    sdk::FfxOpticalflowDispatchDescription dispatch{};
    dispatch.commandList = sdk::ffxGetCommandListDX12(commands);
    dispatch.color = sdk::ffxGetResourceDX12(
        color, sdk::ffxGetResourceDescriptionDX12(color), L"FrameGen packed color",
        sdk::FFX_RESOURCE_STATE_COMPUTE_READ);
    dispatch.opticalFlowVector = sdk::ffxGetResourceDX12(
        state.vectors.Get(), sdk::ffxGetResourceDescriptionDX12(state.vectors.Get()),
        L"FrameGen flow vectors", sdk::FFX_RESOURCE_STATE_UNORDERED_ACCESS);
    dispatch.opticalFlowSCD = sdk::ffxGetResourceDX12(
        state.scene_change.Get(), sdk::ffxGetResourceDescriptionDX12(state.scene_change.Get()),
        L"FrameGen scene change", sdk::FFX_RESOURCE_STATE_UNORDERED_ACCESS);
    dispatch.reset = reset;
    dispatch.backbufferTransferFunction = sdk::FFX_BACKBUFFER_TRANSFER_FUNCTION_SRGB;
    dispatch.minMaxLuminance = {0.0f, 1.0f};
    try {
        const auto result = history_only
            ? sdk::framegenDx12PrepareOpticalFlowHistory(&state.context, &dispatch)
            : sdk::ffxOpticalflowContextDispatch(&state.context, &dispatch);
        state.failed = result != sdk::FFX_OK;
        return state.failed ? E_FAIL : S_OK;
    } catch (...) {
        state.failed = true;
        return E_FAIL;
    }
}

ID3D12Resource* Flow12::vectors() const noexcept { return state_ ? state_->vectors.Get() : nullptr; }
ID3D12Resource* Flow12::scene_change() const noexcept { return state_ ? state_->scene_change.Get() : nullptr; }
UINT Flow12::width() const noexcept { return state_ ? state_->flow_width : 0; }
UINT Flow12::height() const noexcept { return state_ ? state_->flow_height : 0; }

} // namespace actual_amd
