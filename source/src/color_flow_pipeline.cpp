#include "color_flow_pipeline.hpp"
#include "amd/amd_flow.hpp"
#include "amd/stock_camera_d11.hpp"
#include "bgra_blit.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>
#include <optional>
using Microsoft::WRL::ComPtr;
namespace runtime_study {
namespace {
struct SwapState {
    ID3D11DeviceContext* base{};
    ID3D11DeviceContext1* context{};
    ComPtr<ID3DDeviceContextState> saved;
    std::unique_ptr<ComputeState> compute;
    std::unique_ptr<presentation_study::DrawState> draw;
    ID3D11ShaderResourceView* extraPS[6]{};
    std::optional<VertexConstantBuffers<1>> vertexConstants;
    SwapState(ID3D11DeviceContext*c,ID3D11DeviceContext1*c1,ID3DDeviceContextState*privateState):base(c){
        if(c1&&privateState){context=c1;context->SwapDeviceContextState(privateState,&saved);}
        else{
            compute=std::make_unique<ComputeState>(base);
            draw=std::make_unique<presentation_study::DrawState>(base);
            base->PSGetShaderResources(2,6,extraPS);
            vertexConstants.emplace(base);
            ID3D11UnorderedAccessView* uavs[8]{};ID3D11ShaderResourceView* srvs[128]{};
            base->OMSetRenderTargets(0,nullptr,nullptr);
            base->CSSetUnorderedAccessViews(0,8,uavs,nullptr);
            base->CSSetShaderResources(0,128,srvs);
            base->PSSetShaderResources(0,8,srvs);
        }
        base->SetPredication(nullptr,FALSE);
    }
    ~SwapState(){
        if(context){ComPtr<ID3DDeviceContextState> ignored;context->SwapDeviceContextState(saved.Get(),&ignored);}
        else{
            ID3D11UnorderedAccessView* uavs[8]{};ID3D11ShaderResourceView* srvs[128]{};
            base->OMSetRenderTargets(0,nullptr,nullptr);
            base->CSSetUnorderedAccessViews(0,8,uavs,nullptr);
            base->CSSetShaderResources(0,128,srvs);
            base->PSSetShaderResources(0,8,srvs);
            base->PSSetShaderResources(2,6,extraPS);
            vertexConstants->restore();
            for(auto*p:extraPS)if(p)p->Release();
            // DrawState restores graphics outputs/shaders/predication before
            // ComputeState restores the application's valid compute bindings.
            draw.reset();compute.reset();
        }
    }
};
DXGI_FORMAT sourceFormat(DXGI_FORMAT actual,int64_t declared,bool allowLinear){
    for(auto typed:{DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
                    DXGI_FORMAT_B8G8R8A8_UNORM,DXGI_FORMAT_R8G8B8A8_UNORM}){
        const bool linear=typed==DXGI_FORMAT_B8G8R8A8_UNORM||typed==DXGI_FORMAT_R8G8B8A8_UNORM;
        if(linear&&(!allowLinear||declared!=int64_t(typed)))continue;
        if(actual==typed)return !declared||declared==int64_t(typed)?typed:DXGI_FORMAT_UNKNOWN;
        auto family=typed==DXGI_FORMAT_B8G8R8A8_UNORM_SRGB||typed==DXGI_FORMAT_B8G8R8A8_UNORM
            ?DXGI_FORMAT_B8G8R8A8_TYPELESS:DXGI_FORMAT_R8G8B8A8_TYPELESS;
        if(actual==family&&declared==int64_t(typed))return typed;
    }
    return DXGI_FORMAT_UNKNOWN;
}
bool finiteView(const XrCompositionLayerProjectionView&v){
    if(v.type!=XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW)return false;
    const auto&p=v.pose;const float values[]={p.orientation.x,p.orientation.y,p.orientation.z,p.orientation.w,p.position.x,p.position.y,p.position.z,v.fov.angleLeft,v.fov.angleRight,v.fov.angleUp,v.fov.angleDown};
    for(auto n:values)if(!std::isfinite(n))return false;
    float n=p.orientation.x*p.orientation.x+p.orientation.y*p.orientation.y+p.orientation.z*p.orientation.z+p.orientation.w*p.orientation.w;
    return n>.5f&&n<1.5f&&v.fov.angleLeft<v.fov.angleRight&&v.fov.angleDown<v.fov.angleUp&&
        std::abs(v.fov.angleLeft)<1.5707f&&std::abs(v.fov.angleRight)<1.5707f&&std::abs(v.fov.angleUp)<1.5707f&&std::abs(v.fov.angleDown)<1.5707f;
}
bool sameFov(const XrFovf&a,const XrFovf&b){return std::abs(a.angleLeft-b.angleLeft)<1e-6f&&std::abs(a.angleRight-b.angleRight)<1e-6f&&std::abs(a.angleUp-b.angleUp)<1e-6f&&std::abs(a.angleDown-b.angleDown)<1e-6f;}
ofxr_color_d11::View mappingView(const XrCompositionLayerProjectionView&v,UINT w,UINT h){
    ofxr_color_d11::View m;m.orientation={v.pose.orientation.x,v.pose.orientation.y,v.pose.orientation.z,v.pose.orientation.w};m.fov={v.fov.angleLeft,v.fov.angleRight,v.fov.angleUp,v.fov.angleDown};m.rect={0,0,float(w),float(h)};return m;
}
}
struct ColorFlowPipeline::Impl {
    struct Slot {Texture texture;ComPtr<ID3D11ShaderResourceView> srv;};
    struct Eye {
        std::unique_ptr<actual_amd::Flow> flow;
        std::unique_ptr<ofxr_color_d11::Composer> composer;
        Slot history[2];
        Texture output;ComPtr<ID3D11RenderTargetView> outputView;
        ofxr_color_d11::Parameters parameters{};
        UINT width{},height{};DXGI_FORMAT format=DXGI_FORMAT_UNKNOWN;
        XrRect2Di sourceRect{};UINT sourceSlice{};
        bool forward{};
    } eyes[2];
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext4> context;
    ComPtr<ID3D11DeviceContext1> stateContext;ComPtr<ID3DDeviceContextState> privateState;
    ComPtr<ID3D11Fence> fence;UINT64 fenceValue{};
    std::string shader;std::string diagnostics;
    Views previous{};XrTime previousTime{};XrSpace previousSpace=XR_NULL_HANDLE;
    Views captured{};XrTime capturedTime{};XrSpace capturedSpace=XR_NULL_HANDLE;
    int64_t capturedPeriod{};
    unsigned currentSlot{},flowDivisor=2;bool history{},resetRequested{};
    bool repeatControl{},allowLinear{};
    void resetHistory(){history=false;previousTime=0;currentSlot=0;resetRequested=false;}
    bool createEye(Eye&e,UINT w,UINT h,DXGI_FORMAT format,const XrCompositionLayerProjectionView&view,bool forward){
        Eye made;made.width=w;made.height=h;made.format=format;made.sourceRect=view.subImage.imageRect;made.sourceSlice=view.subImage.imageArrayIndex;
        made.forward=forward;
        if(!ofxr_color_d11::set_packed_extent(made.parameters,w,h,1,flowDivisor))return false;
        made.parameters.synthesis_flags=2;
        made.composer=std::make_unique<ofxr_color_d11::Composer>();
        std::string detail;if(FAILED(made.composer->initialize(device.Get(),shader,detail,forward,repeatControl&&forward))){diagnostics+=detail;return false;}
        made.flow=std::make_unique<actual_amd::Flow>();
        if(made.flow->initialize(device.Get(),made.parameters.packed_width,made.parameters.packed_height)!=FFX_OK)return false;
        made.parameters.flow_width=made.flow->width();made.parameters.flow_height=made.flow->height();
        D3D11_TEXTURE2D_DESC td{};td.Width=w;td.Height=h;td.MipLevels=td.ArraySize=td.SampleDesc.Count=1;td.Format=format;td.BindFlags=D3D11_BIND_SHADER_RESOURCE;
        for(auto&slot:made.history){if(FAILED(device->CreateTexture2D(&td,nullptr,&slot.texture)))return false;D3D11_SHADER_RESOURCE_VIEW_DESC sd{};sd.Format=format;sd.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2DARRAY;sd.Texture2DArray.ArraySize=sd.Texture2DArray.MipLevels=1;if(FAILED(device->CreateShaderResourceView(slot.texture.Get(),&sd,&slot.srv)))return false;}
        td.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;td.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET;
        if(FAILED(device->CreateTexture2D(&td,nullptr,&made.output))||FAILED(device->CreateRenderTargetView(made.output.Get(),nullptr,&made.outputView)))return false;
        e=std::move(made);return true;
    }
};
ColorFlowPipeline::ColorFlowPipeline():impl_(std::make_unique<Impl>()){}
ColorFlowPipeline::~ColorFlowPipeline()=default;
bool ColorFlowPipeline::initialize(ID3D11Device*d,ID3D11DeviceContext*c,const std::string&shader,std::string&diagnostics,bool managed){
    auto&s=*impl_;if(!d||!c||shader.empty()||s.device||c->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE)return false;
    ComPtr<ID3D11Device> owner;c->GetDevice(&owner);if(owner.Get()!=d)return false;
    ComPtr<ID3D11Device1>d1;ComPtr<ID3D11Device5>d5;
    if(FAILED(d->QueryInterface(IID_PPV_ARGS(&d5)))||FAILED(c->QueryInterface(IID_PPV_ARGS(&s.context))))return false;
    diagnostics.clear();
#ifndef FRAMEGEN_MENU_LAYER
    char flowDiv[8]{};if(GetEnvironmentVariableA("MOLTEN_FG_FLOW_DIVISOR",flowDiv,sizeof flowDiv)){if(flowDiv[0]=='1'&&flowDiv[1]==0)s.flowDivisor=1;else if(flowDiv[0]=='4'&&flowDiv[1]==0)s.flowDivisor=4;}
    char repeatFlag[8]{};
    s.repeatControl=!managed&&GetEnvironmentVariableA("MOLTEN_FG_REPEAT_CONTROL",repeatFlag,sizeof repeatFlag)==1&&repeatFlag[0]=='1';
#endif
    s.allowLinear=managed;
    // DXMT exposes IMTLDXGIDevice and aborts in SwapDeviceContextState.
    constexpr IID dxmtDeviceId{0x6bfa1657,0x9cb1,0x471a,{0xa4,0xfb,0x7c,0xac,0xf8,0xa8,0x12,0x07}};
    ComPtr<IUnknown> dxmtDevice;
    const bool isDXMT = SUCCEEDED(d->QueryInterface(dxmtDeviceId,
        reinterpret_cast<void**>(dxmtDevice.GetAddressOf()))) && dxmtDevice;
    if (!isDXMT && SUCCEEDED(d->QueryInterface(IID_PPV_ARGS(&d1))) &&
        SUCCEEDED(c->QueryInterface(IID_PPV_ARGS(&s.stateContext)))) {
        auto fl=d->GetFeatureLevel();D3D_FEATURE_LEVEL chosen{};
        if(FAILED(d1->CreateDeviceContextState(0,&fl,1,D3D11_SDK_VERSION,__uuidof(ID3D11Device),&chosen,&s.privateState))){
            s.privateState.Reset();diagnostics="DeviceContextState unavailable; explicit draw/compute state preservation selected.";
        }
    } else if (isDXMT) {
        diagnostics = "DXMT detected; explicit draw/compute state preservation selected.";
    } else {
        diagnostics = "DeviceContextState interfaces unavailable; explicit draw/compute state preservation selected.";
    }
    if(FAILED(d5->CreateFence(0,D3D11_FENCE_FLAG_NONE,IID_PPV_ARGS(&s.fence))))return false;
    s.device=d;s.shader=shader;return true;
}
void ColorFlowPipeline::requestReset()noexcept{impl_->resetRequested=true;impl_->capturedTime=0;}
bool ColorFlowPipeline::generate(const std::array<Texture,2>&source,const Views&current,XrTime sourceTime,const Views&target,Result&result,XrSpace space,bool forceReset,const std::array<int64_t,2>&declaredFormats,int64_t forecastPeriodNs){
    result={};
    return captureSource(source,current,sourceTime,space,forceReset,declaredFormats,forecastPeriodNs)&&dispatchCaptured(target,result);
}
bool ColorFlowPipeline::captureSource(const std::array<Texture,2>&source,const Views&current,XrTime sourceTime,
                                      XrSpace space,bool forceReset,const std::array<int64_t,2>&declaredFormats,
                                      int64_t forecastPeriodNs){
    auto&s=*impl_;s.capturedTime=0;if(!s.device||!s.context||sourceTime<=0)return false;
    if(forecastPeriodNs<0 || forecastPeriodNs>50'000'000)return false;
    const bool forward=forecastPeriodNs>0;
    const auto maxGap=forward?4*forecastPeriodNs:250'000'000;
    D3D11_TEXTURE2D_DESC desc[2]{};UINT width[2]{},height[2]{};DXGI_FORMAT resolvedFormat[2]{};
    bool reset=forceReset||s.resetRequested||!s.history||sourceTime<=s.previousTime||sourceTime-s.previousTime>maxGap||space!=s.previousSpace;
    for(unsigned eye=0;eye<2;eye++){
        if(!source[eye]||!finiteView(current[eye]))return false;
        ComPtr<ID3D11Device> owner;source[eye]->GetDevice(&owner);if(owner.Get()!=s.device.Get())return false;
        source[eye]->GetDesc(&desc[eye]);auto&td=desc[eye];const auto&r=current[eye].subImage.imageRect;
        // Resolve typeless storage only from the original XR declared format.
        // Explicit UNORM stays linear. HDR or unknown storage is passed through.
        resolvedFormat[eye]=sourceFormat(td.Format,declaredFormats[eye],s.allowLinear);
        if(resolvedFormat[eye]==DXGI_FORMAT_UNKNOWN||td.SampleDesc.Count!=1||!td.MipLevels||current[eye].subImage.imageArrayIndex>=td.ArraySize||r.offset.x<0||r.offset.y<0||r.extent.width<128||r.extent.height<128||uint64_t(r.offset.x)+r.extent.width>td.Width||uint64_t(r.offset.y)+r.extent.height>td.Height)return false;
        width[eye]=r.extent.width;height[eye]=r.extent.height;
        if(s.history){auto&e=s.eyes[eye];reset=reset||s.previous[eye].subImage.swapchain!=current[eye].subImage.swapchain||e.width!=width[eye]||e.height!=height[eye]||e.format!=resolvedFormat[eye]||e.forward!=forward||!sameFov(s.previous[eye].fov,current[eye].fov)||e.sourceRect.offset.x!=r.offset.x||e.sourceRect.offset.y!=r.offset.y||e.sourceRect.extent.width!=r.extent.width||e.sourceRect.extent.height!=r.extent.height||e.sourceSlice!=current[eye].subImage.imageArrayIndex;}
    }
    if(reset)s.resetHistory();
    for(unsigned eye=0;eye<2;eye++){
        auto&e=s.eyes[eye];
        // Camera/history cuts do not change allocation dimensions. Reuse all
        // shaders/textures and reset SDK history on the next prime dispatch.
        if((!e.flow||e.width!=width[eye]||e.height!=height[eye]||e.format!=resolvedFormat[eye]||e.forward!=forward)&&!s.createEye(e,width[eye],height[eye],resolvedFormat[eye],current[eye],forward)){s.resetRequested=true;return false;}
        e.sourceRect=current[eye].subImage.imageRect;e.sourceSlice=current[eye].subImage.imageArrayIndex;
    }
    ComPtr<ID3D11Predicate> predicate;BOOL predicateValue=FALSE;
    s.context->GetPredication(&predicate,&predicateValue);
    if(predicate)s.context->SetPredication(nullptr,FALSE);
    for(unsigned eye=0;eye<2;eye++){
        auto&e=s.eyes[eye];const auto&r=current[eye].subImage.imageRect;
        D3D11_BOX box{UINT(r.offset.x),UINT(r.offset.y),0,UINT(r.offset.x+r.extent.width),UINT(r.offset.y+r.extent.height),1};
        const auto sub=D3D11CalcSubresource(0,current[eye].subImage.imageArrayIndex,desc[eye].MipLevels);
        s.context->CopySubresourceRegion(e.history[s.currentSlot].texture.Get(),0,0,0,0,source[eye].Get(),sub,&box);
    }
    if(predicate)s.context->SetPredication(predicate.Get(),predicateValue);
    s.captured=current;for(auto&v:s.captured)v.next=nullptr;
    s.capturedTime=sourceTime;s.capturedSpace=space;s.capturedPeriod=forecastPeriodNs;
    return true;
}
bool ColorFlowPipeline::dispatchCaptured(const Views&target,Result&result){
    result={};auto&s=*impl_;
    const auto sourceTime=std::exchange(s.capturedTime,0);
    if(!sourceTime)return false;
    const auto& current=s.captured;
    const auto space=s.capturedSpace;
    const auto forecastPeriodNs=s.capturedPeriod;
    const bool forward=forecastPeriodNs>0;
    const bool repeat=s.repeatControl&&forward;
    for(unsigned eye=0;eye<2;++eye){
        const auto&a=current[eye];const auto&b=target[eye];
        // The color-only warp has no depth with which to change the eye's
        // optical centre or FOV.
        if(!finiteView(b)||!sameFov(a.fov,b.fov)||std::abs(a.pose.position.x-b.pose.position.x)>1e-5f||
           std::abs(a.pose.position.y-b.pose.position.y)>1e-5f||std::abs(a.pose.position.z-b.pose.position.z)>1e-5f){
            s.resetRequested=true;return false;
        }
    }
    SwapState state(s.context.Get(),s.stateContext.Get(),s.privateState.Get());
    const unsigned now=s.currentSlot,old=1-now;bool first=!s.history;
    for(unsigned eye=0;eye<2;eye++){
        auto&e=s.eyes[eye];
        auto p=e.parameters;const auto previous=first?current[eye]:s.previous[eye];
        if(!ofxr_color_d11::make_mapping(mappingView(previous,e.width,e.height),mappingView(current[eye],e.width,e.height),p.previous_mappings[0])){s.resetRequested=true;return false;}
        p.previous_mappings[1]=p.previous_mappings[0];
        auto* previousSrv=e.history[first?now:old].srv.Get();auto* currentSrv=e.history[now].srv.Get();
        // The diagnostic control still draws into the same output texture and
        // uses the same XR materializer. It removes only the optical-flow work.
        if(!repeat){
            if(FAILED(e.composer->pack_aligned(s.context.Get(),previousSrv,currentSrv,p))){s.resetRequested=true;return false;}
            if(!first&&e.flow->prepare_history(s.context.Get(),e.composer->packed_previous(),false)!=FFX_OK){s.resetRequested=true;return false;}
            for(unsigned prime=0;prime<(first?6u:1u);prime++)if(e.flow->dispatch(s.context.Get(),e.composer->packed_color(),first&&prime==0)!=FFX_OK){s.resetRequested=true;return false;}
        }
        if(!first){
            const float phase=forward?std::min(float(forecastPeriodNs)/float(sourceTime-s.previousTime),.5f):.5f;
            p.synthesis_flags=2u|ofxr_color_d11::fraction_flags(phase);
            if(!forward){
                p.synthesis_flags|=16u;
                ofxr_color_d11::CameraMapping output;
                if(!ofxr_color_d11::make_mapping(mappingView(current[eye],e.width,e.height),mappingView(target[eye],e.width,e.height),output)){s.resetRequested=true;return false;}
                p.previous_mappings[1].target_to_source_rotation=output.target_to_source_rotation;
            }
            D3D11_VIEWPORT vp{0,0,float(e.width),float(e.height),0,1};
            if(FAILED(e.composer->synthesize(s.context.Get(),previousSrv,currentSrv,
                repeat?nullptr:e.flow->vectors(),repeat?nullptr:e.flow->scene_change(),e.outputView.Get(),p,vp))){s.resetRequested=true;return false;}
            result.textures[eye]=e.output;
        }
    }
    s.previous=current;for(auto&v:s.previous)v.next=nullptr;s.previousTime=sourceTime;s.previousSpace=space;s.history=true;s.currentSlot=old;
    result.status=first?Result::warmup:Result::generated;result.sourceTime=sourceTime;result.space=space;
    result.repeated=repeat&&!first;
    return true;
}
HRESULT ColorFlowPipeline::drainGpu(DWORD timeoutMs)noexcept{
    auto&s=*impl_;if(!s.context||!s.fence)return E_UNEXPECTED;
    const auto value=++s.fenceValue;auto hr=s.context->Signal(s.fence.Get(),value);if(FAILED(hr))return hr;s.context->Flush();
    auto done=s.fence->GetCompletedValue();if(done==UINT64_MAX)return DXGI_ERROR_DEVICE_REMOVED;if(done>=value)return S_OK;
    HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);if(!event)return HRESULT_FROM_WIN32(GetLastError());
    hr=s.fence->SetEventOnCompletion(value,event);if(SUCCEEDED(hr)){DWORD waited=WaitForSingleObject(event,std::min<DWORD>(timeoutMs,2000));hr=waited==WAIT_OBJECT_0?S_OK:waited==WAIT_TIMEOUT?HRESULT_FROM_WIN32(WAIT_TIMEOUT):HRESULT_FROM_WIN32(GetLastError());}CloseHandle(event);return hr;
}
}
