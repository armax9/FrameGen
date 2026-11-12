#include "amd_flow.hpp"
#include <FidelityFX/host/ffx_error.h>
#include <stdexcept>
using Microsoft::WRL::ComPtr;
extern "C" FfxErrorCode ffxOpticalflowContextPrepareHistoryPrivate(FfxOpticalflowContext*,const FfxOpticalflowDispatchDescription*);
namespace actual_amd {
std::mutex Flow::mutex_;
Flow::~Flow(){std::lock_guard lock(mutex_);if(initialized_)ffxOpticalflowContextDestroy(&context_);}
FfxErrorCode Flow::initialize(ID3D11Device*d,unsigned w,unsigned h){
 std::lock_guard lock(mutex_);if(!d||!w||!h||initialized_)return FFX_ERROR_INVALID_ARGUMENT;
 scratch_.resize(ffxGetScratchMemorySizeDX11(FFX_OPTICALFLOW_CONTEXT_COUNT));
 FfxOpticalflowContextDescription cd{};cd.resolution={w,h};
 auto e=ffxGetInterfaceDX11(&cd.backendInterface,ffxGetDeviceDX11_Fsr31(d),scratch_.data(),scratch_.size(),FFX_OPTICALFLOW_CONTEXT_COUNT);if(e)return e;
 e=ffxOpticalflowContextCreate(&context_,&cd);if(e)return e;initialized_=true;device_=d;
 FfxOpticalflowSharedResourceDescriptions resources{};e=ffxOpticalflowGetSharedResourceDescriptions(&context_,&resources);if(e)return e;
 auto create=[&](const FfxResourceDescription& desc,ComPtr<ID3D11Texture2D>&t,ComPtr<ID3D11ShaderResourceView>&v){
  D3D11_TEXTURE2D_DESC td{};td.Width=desc.width;td.Height=desc.height;td.MipLevels=td.ArraySize=td.SampleDesc.Count=1;
  td.Format=desc.format==FFX_SURFACE_FORMAT_R16G16_SINT?DXGI_FORMAT_R16G16_SINT:desc.format==FFX_SURFACE_FORMAT_R32_UINT?DXGI_FORMAT_R32_UINT:DXGI_FORMAT_UNKNOWN;
  td.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS;
  if(td.Format==DXGI_FORMAT_UNKNOWN)return E_INVALIDARG;
  auto hr=d->CreateTexture2D(&td,nullptr,&t);return FAILED(hr)?hr:d->CreateShaderResourceView(t.Get(),nullptr,&v);
 };
 if(FAILED(create(resources.opticalFlowVector.resourceDescription,flow_,flow_srv_))||FAILED(create(resources.opticalFlowSCD.resourceDescription,scd_,scd_srv_)))return FFX_ERROR_BACKEND_API_ERROR;
 width_=resources.opticalFlowVector.resourceDescription.width;height_=resources.opticalFlowVector.resourceDescription.height;return FFX_OK;
}
FfxErrorCode Flow::dispatch(ID3D11DeviceContext*c,ID3D11Texture2D*color,bool reset){return dispatch_impl(c,color,reset,false);}
FfxErrorCode Flow::prepare_history(ID3D11DeviceContext*c,ID3D11Texture2D*color,bool reset){return dispatch_impl(c,color,reset,true);}
FfxErrorCode Flow::dispatch_impl(ID3D11DeviceContext*c,ID3D11Texture2D*color,bool reset,bool historyOnly){
 std::lock_guard lock(mutex_);if(!initialized_||!c||!color||!flow_||!scd_)return FFX_ERROR_INVALID_ARGUMENT;
 ComPtr<ID3D11Device>d;c->GetDevice(&d);if(d.Get()!=device_.Get())return FFX_ERROR_INVALID_ARGUMENT;color->GetDevice(&d);if(d.Get()!=device_.Get())return FFX_ERROR_INVALID_ARGUMENT;
 FfxOpticalflowDispatchDescription p{};p.commandList=ffxGetCommandListDX11(c);
 p.color=ffxGetResourceDX11_Fsr31(color,GetFfxResourceDescriptionDX11(color),L"Color");
 p.opticalFlowVector=ffxGetResourceDX11_Fsr31(flow_.Get(),GetFfxResourceDescriptionDX11(flow_.Get()),L"Flow",FFX_RESOURCE_STATE_UNORDERED_ACCESS);
 p.opticalFlowSCD=ffxGetResourceDX11_Fsr31(scd_.Get(),GetFfxResourceDescriptionDX11(scd_.Get()),L"SceneChange",FFX_RESOURCE_STATE_UNORDERED_ACCESS);
 p.reset=reset;p.backbufferTransferFunction=FFX_BACKBUFFER_TRANSFER_FUNCTION_SRGB;p.minMaxLuminance={0,1};
 return historyOnly?ffxOpticalflowContextPrepareHistoryPrivate(&context_,&p):ffxOpticalflowContextDispatch(&context_,&p);
}
}
