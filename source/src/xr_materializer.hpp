#pragma once
#include "gpu_publications.hpp"
#include "bgra_blit.hpp"
#include "xrfg/context_encoding_gate.hpp"
#include <algorithm>
#ifndef XR_USE_GRAPHICS_API_D3D11
#define XR_USE_GRAPHICS_API_D3D11
#endif
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <vector>
namespace presentation_study {
struct ImageDispatch {
 XrSession session=XR_NULL_HANDLE;
 PFN_xrEnumerateSwapchainFormats formats=nullptr;PFN_xrCreateSwapchain create=nullptr;PFN_xrDestroySwapchain destroy=nullptr;
 PFN_xrEnumerateSwapchainImages enumerate=nullptr;PFN_xrAcquireSwapchainImage acquire=nullptr;PFN_xrWaitSwapchainImage wait=nullptr;PFN_xrReleaseSwapchainImage release=nullptr;
};
class Materializer {
 struct Chain {
  XrSwapchain handle=XR_NULL_HANDLE;
  std::vector<XrSwapchainImageD3D11KHR> images;
  std::vector<std::array<ComPtr<ID3D11RenderTargetView>,2>> rtvs;
  UINT w=0,h=0,arraySize=0,index=0;
  bool acquired=false,waited=false;
 };
 struct Retired {uint64_t fence=0;std::shared_ptr<const Images>source;};
 ImageDispatch api_;ComPtr<ID3D11Device>device_;ComPtr<ID3D11DeviceContext4>context_;ComPtr<ID3D11Fence>fence_;uint64_t serial_=0;int64_t format_=0;bool fault_=false;
 D3D11ColorBlit blit_;
 xrfg::ContextEncodingGate* opaqueEncoding_=nullptr;
 std::vector<int64_t> formats_;
 Chain eyes_[2];Retired retired_[3];
 static bool isSdr(int64_t format){
  return format==DXGI_FORMAT_B8G8R8A8_UNORM_SRGB||format==DXGI_FORMAT_B8G8R8A8_UNORM||
         format==DXGI_FORMAT_R8G8B8A8_UNORM_SRGB||format==DXGI_FORMAT_R8G8B8A8_UNORM;
 }
 // Source draw fences protect our views/leases; destroy additionally drains
 // this exact old proxy chain's queued native End consumers. Never reuse a
 // different-sized chain or clear its GPU references without both proofs.
 bool resizeChains(const std::array<UINT,2>&widths,const std::array<UINT,2>&heights,uint64_t completed,
                   bool formatChanged=false,bool arrayStereo=false){
  bool changed=formatChanged;
  for(unsigned eye=0;eye<2;++eye){
   const auto& chain=eyes_[eye];
   const UINT arraySize=arrayStereo?(eye==0?2u:0u):1u;
   if(chain.handle&&(chain.w!=widths[eye]||chain.h!=heights[eye]||chain.arraySize!=arraySize))
    changed=true;
  }
  if(!changed)return true;if(completed<serial_)return false;
  // A prior zero-timeout Wait may have left an acquisition outstanding. Poll
  // once and retry a later frame rather than spin or destroy a pending image.
  for(auto&chain:eyes_)if(chain.acquired&&!chain.waited){XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};wi.timeout=0;auto result=api_.wait(chain.handle,&wi);if(result==XR_TIMEOUT_EXPIRED)return false;if(result!=XR_SUCCESS){fault_=true;return false;}chain.waited=true;}
  releaseWaited();if(fault_)return false;
  for(auto&chain:eyes_)if(chain.handle){if(api_.destroy(chain.handle)!=XR_SUCCESS){fault_=true;return false;}chain=Chain{};}
  for(auto&r:retired_)r=Retired{};
  blit_.clearSourceViews();
  return true;
 }
 bool make(Chain& chain,UINT w,UINT h,UINT arraySize){
  if(chain.handle)return chain.w==w&&chain.h==h&&chain.arraySize==arraySize;
  Chain created;
  XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
  info.usageFlags=XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT|XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
  info.format=format_;
  info.sampleCount=info.faceCount=info.mipCount=1;
  info.arraySize=arraySize;
  info.width=w;
  info.height=h;
  if(api_.create(api_.session,&info,&created.handle)!=XR_SUCCESS)return false;
  auto fail=[&]{
   if(api_.destroy(created.handle)!=XR_SUCCESS){
    chain.handle=created.handle;
    fault_=true;
   }
   return false;
  };
  uint32_t count=0;
  if(api_.enumerate(created.handle,0,&count,nullptr)!=XR_SUCCESS||!count)return fail();
  created.images.resize(count);
  for(auto& image:created.images)image.type=XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR;
  if(api_.enumerate(created.handle,count,&count,
      reinterpret_cast<XrSwapchainImageBaseHeader*>(created.images.data()))!=XR_SUCCESS)return fail();
  if(isSdr(format_)){
   created.rtvs.resize(count);
   D3D11_RENDER_TARGET_VIEW_DESC view{};
   view.Format=DXGI_FORMAT(format_);
   view.ViewDimension=arraySize==1?D3D11_RTV_DIMENSION_TEXTURE2D:D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
   for(uint32_t index=0;index<count;++index){
    for(UINT slice=0;slice<arraySize;++slice){
     if(arraySize>1)view.Texture2DArray={0,slice,1};
     if(FAILED(device_->CreateRenderTargetView(created.images[index].texture,&view,&created.rtvs[index][slice])))
      return fail();
    }
   }
  }
  created.w=w;
  created.h=h;
  created.arraySize=arraySize;
  chain=std::move(created);
  return true;
 }
 bool acquireWaited(Chain& chain){
  if(!chain.acquired){
   XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
   if(api_.acquire(chain.handle,&acquire,&chain.index)!=XR_SUCCESS)return false;
   chain.acquired=true;
  }
  if(chain.index>=chain.images.size()){fault_=true;return false;}
  if(!chain.waited){
   XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
   wait.timeout=0;
   const auto result=api_.wait(chain.handle,&wait);
   if(result==XR_TIMEOUT_EXPIRED)return false;
   if(result!=XR_SUCCESS){fault_=true;return false;}
   chain.waited=true;
  }
  return true;
 }
 static bool sameTextureDescription(const D3D11_TEXTURE2D_DESC& a,const D3D11_TEXTURE2D_DESC& b){
  return a.Width==b.Width&&a.Height==b.Height&&a.MipLevels==b.MipLevels&&a.ArraySize==b.ArraySize&&
         a.Format==b.Format&&a.SampleDesc.Count==b.SampleDesc.Count&&
         a.SampleDesc.Quality==b.SampleDesc.Quality&&a.Usage==b.Usage&&a.BindFlags==b.BindFlags&&
         a.CPUAccessFlags==b.CPUAccessFlags&&a.MiscFlags==b.MiscFlags;
 }
 bool copy(Chain& chain,ID3D11Texture2D* source,const RotationWarp* warp,
           ID3D11Texture2D* fallback,D3D11ColorBlit::Batch* batch,unsigned eye,
           const D3D11_TEXTURE2D_DESC* expected){
  auto* target=chain.images[chain.index].texture;
  if(!target)return false;
  D3D11_TEXTURE2D_DESC desc{};
  target->GetDesc(&desc);
  const UINT slice=chain.arraySize==2?eye:0;
  if(expected&&!sameTextureDescription(desc,*expected))return false;
  if(desc.Width!=chain.w||desc.Height!=chain.h||desc.ArraySize!=chain.arraySize||
     desc.MipLevels!=1||desc.SampleDesc.Count!=1)return false;
  if(isSdr(format_))
   return blit_.copy(device_.Get(),context_.Get(),source,target,DXGI_FORMAT(format_),
                     chain.w,chain.h,warp,fallback,batch,chain.rtvs[chain.index][slice].Get(),eye);
  if(format_!=DXGI_FORMAT_R16G16B16A16_FLOAT||
     (desc.Format!=DXGI_FORMAT_R16G16B16A16_FLOAT&&desc.Format!=DXGI_FORMAT_R16G16B16A16_TYPELESS))
   return false;
  ComPtr<ID3D11Predicate> predicate;
  BOOL value=FALSE;
  context_->GetPredication(&predicate,&value);
  if(predicate)context_->SetPredication(nullptr,FALSE);
  if(chain.arraySize==1)context_->CopyResource(target,source);
  else context_->CopySubresourceRegion(target,slice,0,0,0,source,0,nullptr);
  if(predicate)context_->SetPredication(predicate.Get(),value);
  return true;
 }
 void releaseWaited(){for(auto&chain:eyes_)if(chain.acquired&&chain.waited){XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};if(api_.release(chain.handle,&ri)!=XR_SUCCESS)fault_=true;else chain.acquired=chain.waited=false;}}
public:
 ~Materializer(){for(auto&chain:eyes_)if(chain.handle&&api_.destroy)api_.destroy(chain.handle);}
 bool initialize(ID3D11Device* d,ID3D11DeviceContext* c,const ImageDispatch& api,
                 xrfg::ContextEncodingGate* opaqueEncoding=nullptr,bool matchSource=false){
  opaqueEncoding_=opaqueEncoding;
  if(!d||!c||!api.session||!api.formats||!api.create||!api.destroy||!api.enumerate||
     !api.acquire||!api.wait||!api.release)return false;
  api_=api;
  device_=d;
  ComPtr<ID3D11Device5> d5;
  if(FAILED(d->QueryInterface(IID_PPV_ARGS(&d5)))||FAILED(c->QueryInterface(IID_PPV_ARGS(&context_)))||
     FAILED(d5->CreateFence(0,D3D11_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence_))))return false;
  uint32_t count=0;
  if(api.formats(api.session,0,&count,nullptr)!=XR_SUCCESS||!count)return false;
  formats_.resize(count);
  if(api.formats(api.session,count,&count,formats_.data())!=XR_SUCCESS)return false;
  formats_.resize(count);
  if(matchSource)
   return std::any_of(formats_.begin(),formats_.end(),isSdr)&&blit_.initialize(d);
  for(auto preferred:{DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,DXGI_FORMAT_B8G8R8A8_UNORM,
                      DXGI_FORMAT_R16G16B16A16_FLOAT,DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
                      DXGI_FORMAT_R8G8B8A8_UNORM}){
   if(std::find(formats_.begin(),formats_.end(),preferred)==formats_.end())continue;
   format_=preferred;
   return !isSdr(format_)||blit_.initialize(d);
  }
  return false;
 }
 bool supportsSourceFormat(int64_t format)const{
  return isSdr(format)&&std::find(formats_.begin(),formats_.end(),format)!=formats_.end();
 }
 bool selectSourceFormat(int64_t format){
  if(fault_||!supportsSourceFormat(format))return false;
  if(format_==format)return true;
  const auto completed=fence_->GetCompletedValue();
  if(completed==UINT64_MAX){fault_=true;return false;}
  if(!resizeChains({},{},completed,true))return false;
  format_=format;
  return true;
 }
 // Allocate the output chains while history is priming. The next source's
 // optional forecast has a strict deadline and should not pay first-use setup.
 void prime(const std::array<UINT,2>&widths,const std::array<UINT,2>&heights,bool arrayStereo=false){
  if(fault_||!fence_||!format_)return;
  if(arrayStereo&&(widths[0]!=widths[1]||heights[0]!=heights[1]))return;
  const auto completed=fence_->GetCompletedValue();
  if(completed==UINT64_MAX){fault_=true;return;}
  if(!resizeChains(widths,heights,completed,false,arrayStereo))return;
  for(unsigned eye=0;eye<(arrayStereo?1u:2u);++eye)
   if(!make(eyes_[eye],widths[eye],heights[eye],arrayStereo?2u:1u))return;
 }
 bool materialize(const Plan&plan,std::array<XrSwapchainSubImage,2>&out,const Stereo*sourceCamera=nullptr,const Stereo*targetCamera=nullptr,ID3D11Texture2D*const*fallback=nullptr,const std::array<XrExtent2Di,2>*outputExtents=nullptr,
                  const std::array<D3D11_TEXTURE2D_DESC,2>*expected=nullptr,bool arrayStereo=false){
  if(fault_||!fence_||!format_)return false;auto images=std::dynamic_pointer_cast<const GpuImages>(plan.images);if(!images||(!images->gpuComplete&&images->orderedContext!=context_.Get())||images->generation!=plan.sequence)return false;
  auto done=fence_->GetCompletedValue();if(done==UINT64_MAX){fault_=true;return false;}
  const bool cameraWarp=sourceCamera&&targetCamera&&fallback;
  const bool sdr=isSdr(format_);
  // Only the SDR camera warp uses normalized sampling of both images. Keep
  // the generator's color, depth and motion in their original pixel grid.
  if(outputExtents&&(!cameraWarp||!sdr))return false;
  std::array<UINT,2>widths{},heights{};
  for(unsigned e=0;e<2;e++){
   auto*source=images->eye(e);if(!source)return false;D3D11_TEXTURE2D_DESC td{};source->GetDesc(&td);if(!td.Width||!td.Height||td.Format!=DXGI_FORMAT_R16G16B16A16_FLOAT||td.ArraySize!=1||td.SampleDesc.Count!=1)return false;
   widths[e]=td.Width;heights[e]=td.Height;
   if(outputExtents){
    const auto extent=(*outputExtents)[e];
    if(extent.width<int32_t(td.Width)||extent.height<int32_t(td.Height))return false;
    widths[e]=UINT(extent.width);heights[e]=UINT(extent.height);
   }
  }
  if(arrayStereo&&(widths[0]!=widths[1]||heights[0]!=heights[1]))return false;
  if(!resizeChains(widths,heights,done,false,arrayStereo))return false;
  Retired*credit=nullptr;for(auto&r:retired_){if(r.source&&r.fence<=done)r.source.reset();if(!r.source&&!credit)credit=&r;}if(!credit)return false;
  for(unsigned eye=0;eye<(arrayStereo?1u:2u);++eye){
   auto& chain=eyes_[eye];
   if(!make(chain,widths[eye],heights[eye],arrayStereo?2u:1u)||!acquireWaited(chain))return false;
  }
  credit->source=images;
  RotationWarp warps[2]{};
  if(cameraWarp){if(!sdr){releaseWaited();return false;}for(unsigned e=0;e<2;e++)if(!rotationWarp(sourceCamera->eyes[e],targetCamera->eyes[e],warps[e])){releaseWaited();return false;}}
  std::unique_lock<xrfg::ContextEncodingGate> encoding;if(opaqueEncoding_)encoding=std::unique_lock<xrfg::ContextEncodingGate>(*opaqueEncoding_);
  auto batch=sdr?blit_.beginBatch(context_.Get()):nullptr;
  bool copied=true;
  for(unsigned eye=0;eye<2&&copied;++eye)
   copied=copy(eyes_[arrayStereo?0:eye],images->eye(eye),cameraWarp?&warps[eye]:nullptr,
               cameraWarp?fallback[eye]:nullptr,batch.get(),eye,expected?&(*expected)[eye]:nullptr);
  batch.reset();credit->fence=++serial_;if(FAILED(context_->Signal(fence_.Get(),serial_))){credit->fence=UINT64_MAX;fault_=true;return false;}context_->Flush();
  if(encoding.owns_lock())encoding.unlock();
  releaseWaited();if(fault_||!copied)return false;
  for(unsigned eye=0;eye<2;++eye){
   out[eye].swapchain=eyes_[arrayStereo?0:eye].handle;
   out[eye].imageArrayIndex=arrayStereo?eye:0;
   out[eye].imageRect={{0,0},{int32_t(widths[eye]),int32_t(heights[eye])}};
  }
  return true;
 }
 bool idle()const{if(!fence_)return true;auto value=fence_->GetCompletedValue();return value!=UINT64_MAX&&value>=serial_;}
 int64_t format()const{return format_;}
};
}
