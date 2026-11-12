#pragma once
#include "presentation_policy.hpp"
#include <d3d11_4.h>
#include <wrl/client.h>
#include <optional>
namespace presentation_study {
using Microsoft::WRL::ComPtr;
struct GpuStorage {ComPtr<ID3D11Texture2D>texture[4],source[4];uint64_t fence=0;bool pending=false,hasSynthetic=false;Pair metadata;};
struct GpuImages final:Images {ID3D11DeviceContext*orderedContext=nullptr;std::shared_ptr<const GpuStorage>owner;unsigned offset=0;ID3D11Texture2D*eye(unsigned index)const{return index<2?owner->texture[offset+index].Get():nullptr;}};
// Single-owner immediate-context transport. Copies only GPU->GPU. Caller must
// serialize all use of this same context with the game/other producer threads.
class PublicationRing {
 ComPtr<ID3D11Device5>device_;ComPtr<ID3D11DeviceContext4>context_;ComPtr<ID3D11Fence>fence_;
 std::array<std::shared_ptr<GpuStorage>,3>slots_{};uint64_t serial_=0;
 bool texture(GpuStorage&s,unsigned index,ID3D11Texture2D*source){D3D11_TEXTURE2D_DESC d{},old{};source->GetDesc(&d);if(s.texture[index]){s.texture[index]->GetDesc(&old);if(old.Width!=d.Width||old.Height!=d.Height||old.Format!=d.Format)s.texture[index].Reset();}if(!s.texture[index]){d.Usage=D3D11_USAGE_DEFAULT;d.CPUAccessFlags=d.MiscFlags=0;d.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET;d.MipLevels=1;if(FAILED(device_->CreateTexture2D(&d,nullptr,&s.texture[index])))return false;}return true;}
public:
 bool initialize(ID3D11Device*d,ID3D11DeviceContext*c){if(!d||!c||c->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE)return false;ComPtr<ID3D11Device>owner;c->GetDevice(&owner);if(owner.Get()!=d)return false;if(FAILED(d->QueryInterface(IID_PPV_ARGS(&device_)))||FAILED(c->QueryInterface(IID_PPV_ARGS(&context_)))||FAILED(device_->CreateFence(0,D3D11_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence_))))return false;for(auto&s:slots_)s=std::make_shared<GpuStorage>();return true;}
 bool enqueue(const Pair&metadata,ID3D11Texture2D*const (&sources)[4]){
  if(!fence_||!metadata.epoch||!metadata.sequence||!valid(metadata.current))return false;uint64_t done=fence_->GetCompletedValue();if(done==UINT64_MAX)return false;
  bool synthetic=sources[0]&&sources[1];if(!sources[2]||!sources[3]||(bool(sources[0])!=bool(sources[1])))return false;
  for(unsigned i=0;i<4;i++)if(sources[i]){ComPtr<ID3D11Device>owner;sources[i]->GetDevice(&owner);D3D11_TEXTURE2D_DESC d{};sources[i]->GetDesc(&d);if(owner.Get()!=device_.Get()||!d.Width||!d.Height||d.MipLevels!=1||d.ArraySize!=1||d.SampleDesc.Count!=1||d.Format!=DXGI_FORMAT_R16G16B16A16_FLOAT)return false;}
  std::shared_ptr<GpuStorage>s;for(auto&candidate:slots_)if(candidate.use_count()==1&&!candidate->pending&&candidate->fence<=done){s=candidate;break;}if(!s)return false;
  for(unsigned i=0;i<4;i++)if(sources[i]&&!texture(*s,i,sources[i]))return false;
  s->hasSynthetic=synthetic;s->metadata=metadata;s->metadata.real.reset();s->metadata.synthetic.reset();
  for(unsigned i=0;i<4;i++){s->source[i]=sources[i];if(sources[i])context_->CopyResource(s->texture[i].Get(),sources[i]);}
  uint64_t marker=++serial_;HRESULT hr=context_->Signal(fence_.Get(),marker);if(FAILED(hr)){s->fence=UINT64_MAX;s->pending=false;return false;}
  s->fence=marker;s->pending=true;context_->Flush();return true;
 }
 bool idle()const{if(!fence_)return false;auto done=fence_->GetCompletedValue();return done!=UINT64_MAX&&done>=serial_;}
 std::shared_ptr<const Pair>takeLatestReady(uint64_t epoch){
  if(!fence_)return{};auto done=fence_->GetCompletedValue();if(done==UINT64_MAX)return{};std::shared_ptr<GpuStorage>best;
  for(auto&s:slots_)if(s->pending&&s->fence<=done){for(auto&source:s->source)source.Reset();if(s->metadata.epoch==epoch&&(!best||s->metadata.sequence>best->metadata.sequence))best=s;}
  if(!best)return{};
  auto p=std::make_shared<Pair>(best->metadata);auto image=[&](unsigned offset){auto im=std::make_shared<GpuImages>();im->generation=p->sequence;im->gpuComplete=true;im->owner=best;im->offset=offset;return im;};p->real=image(2);if(best->hasSynthetic)p->synthetic=image(0);
  for(auto&s:slots_)if(s->pending&&s->fence<=done&&(s->metadata.epoch!=epoch||s->metadata.sequence<=best->metadata.sequence))s->pending=false;
  return p;
 }
 void discardEpoch(uint64_t epoch){if(!fence_)return;auto done=fence_->GetCompletedValue();if(done==UINT64_MAX)return;for(auto&s:slots_)if(s->pending&&s->metadata.epoch!=epoch&&s->fence<=done){s->pending=false;for(auto&source:s->source)source.Reset();}}
};
}
