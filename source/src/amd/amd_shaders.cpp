#include <cstring>
#include <FidelityFX/host/ffx_opticalflow.h>
#include "ffx_shader_blobs.h"
#include "embedded_opticalflow.hpp"

// The fixed, validated DX11 path uses SM5/FP32 and emulates group reductions.
// FP16/Wave64 preferences may select this conservative variant, exactly as the
// former runtime compiler did. HDR transfer remains a runtime shader input.
// No HLSL source file, filesystem lookup, reflection or compiler is needed here.
namespace {
constexpr uint32_t knownOpticalFlowPermutations=0x7u;
bool validOptions(FfxEffect effect,uint32_t options) noexcept {
    return effect==FFX_EFFECT_OPTICALFLOW&&(options&~knownOpticalFlowPermutations)==0;
}
}
extern "C" FfxErrorCode ffxIsWave64(FfxEffect effect,uint32_t options,bool& result) {
    result=false;
    return validOptions(effect,options)?FFX_OK:FFX_ERROR_INVALID_ARGUMENT;
}
extern "C" FfxErrorCode ffxGetPermutationBlobByIndex(
    FfxEffect effect,FfxPass pass,FfxBindStage stage,uint32_t options,FfxShaderBlob* output) {
    if(!output)return FFX_ERROR_INVALID_POINTER;
    if(!validOptions(effect,options)||pass>=7||stage!=FFX_BIND_COMPUTE_SHADER_STAGE)
        return FFX_ERROR_INVALID_ARGUMENT;
    const auto& blob=amd_embedded::blobs[pass];
    if(!blob.data||blob.size<4||std::memcmp(blob.data,"DXBC",4)!=0)
        return FFX_ERROR_BACKEND_API_ERROR;
    std::memcpy(output,&blob,sizeof(blob));
    return FFX_OK;
}
