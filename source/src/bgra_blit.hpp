#pragma once
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <memory>
#include <array>
#include "compute_state.hpp"
#include "presentation_policy.hpp"
namespace presentation_study {
struct RotationWarp {float rows[12]{},tangents[4]{};};
inline bool rotationWarp(const Eye&source,const Eye&target,RotationWarp&out){
 if(!valid(source.pose.q)||!valid(target.pose.q))return false;for(int k=0;k<4;k++)if(std::fabs(source.fov[k]-target.fov[k])>1e-6f)return false;
 Q q=mul(conjugate(source.pose.q),target.pose.q);float x=q.x,y=q.y,z=q.z,w=q.w;
 out={{1-2*(y*y+z*z),2*(x*y-z*w),2*(x*z+y*w),0,2*(x*y+z*w),1-2*(x*x+z*z),2*(y*z-x*w),0,2*(x*z-y*w),2*(y*z+x*w),1-2*(x*x+y*y),0},{std::tan(source.fov[0]),std::tan(source.fov[1]),std::tan(source.fov[2]),std::tan(source.fov[3])}};return true;
}
struct DrawState {
 ID3D11DeviceContext*c;ID3D11RenderTargetView*rt[8]{};ID3D11DepthStencilView*ds=nullptr;ID3D11UnorderedAccessView*uav[8]{};
 ID3D11BlendState*blend=nullptr;FLOAT factor[4]{};UINT sample=0;ID3D11DepthStencilState*depth=nullptr;UINT stencil=0;ID3D11RasterizerState*raster=nullptr;
 D3D11_VIEWPORT vp[16]{};UINT nvp=16;D3D11_RECT rect[16]{};UINT nr=16;ID3D11InputLayout*layout=nullptr;D3D11_PRIMITIVE_TOPOLOGY topology{};
 ID3D11VertexShader*vs=nullptr;ID3D11PixelShader*ps=nullptr;ID3D11GeometryShader*gs=nullptr;ID3D11HullShader*hs=nullptr;ID3D11DomainShader*dsShader=nullptr;
 ID3D11ClassInstance*vc[256]{},*pc[256]{},*gc[256]{},*hc[256]{},*dc[256]{};UINT nv=256,np=256,ng=256,nh=256,nd=256;
 ID3D11ShaderResourceView*srv[2]{};PixelConstantBuffers<1> constants;ID3D11SamplerState*sampler=nullptr;ID3D11Predicate*predicate=nullptr;BOOL pred=FALSE;
 explicit DrawState(ID3D11DeviceContext*x):c(x),constants(x){c->OMGetRenderTargets(8,rt,&ds);c->OMGetRenderTargetsAndUnorderedAccessViews(0,nullptr,nullptr,0,8,uav);c->OMGetBlendState(&blend,factor,&sample);c->OMGetDepthStencilState(&depth,&stencil);c->RSGetState(&raster);c->RSGetViewports(&nvp,vp);c->RSGetScissorRects(&nr,rect);c->IAGetInputLayout(&layout);c->IAGetPrimitiveTopology(&topology);c->VSGetShader(&vs,vc,&nv);c->PSGetShader(&ps,pc,&np);c->GSGetShader(&gs,gc,&ng);c->HSGetShader(&hs,hc,&nh);c->DSGetShader(&dsShader,dc,&nd);c->PSGetShaderResources(0,2,srv);c->PSGetSamplers(0,1,&sampler);c->GetPredication(&predicate,&pred);}
 ~DrawState(){ID3D11ShaderResourceView*none[2]{};c->PSSetShaderResources(0,2,none);c->OMSetRenderTargets(0,nullptr,nullptr);c->VSSetShader(vs,vc,nv);c->PSSetShader(ps,pc,np);c->GSSetShader(gs,gc,ng);c->HSSetShader(hs,hc,nh);c->DSSetShader(dsShader,dc,nd);c->IASetInputLayout(layout);c->IASetPrimitiveTopology(topology);c->RSSetState(raster);c->RSSetViewports(nvp,vp);c->RSSetScissorRects(nr,rect);c->OMSetBlendState(blend,factor,sample);c->OMSetDepthStencilState(depth,stencil);UINT count=0;for(UINT k=0;k<8;k++)if(rt[k])count=k+1;UINT keep[8];for(auto&x:keep)x=UINT(-1);c->OMSetRenderTargetsAndUnorderedAccessViews(count,rt,ds,count,8-count,uav+count,keep+count);c->PSSetShaderResources(0,2,srv);constants.restore();c->PSSetSamplers(0,1,&sampler);c->SetPredication(predicate,pred);
 auto release=[](auto*p){if(p)p->Release();};for(auto*p:rt)release(p);for(auto*p:uav)release(p);release(ds);release(blend);release(depth);release(raster);release(layout);release(vs);release(ps);release(gs);release(hs);release(dsShader);for(auto*p:srv)release(p);release(sampler);release(predicate);for(UINT k=0;k<nv;k++)release(vc[k]);for(UINT k=0;k<np;k++)release(pc[k]);for(UINT k=0;k<ng;k++)release(gc[k]);for(UINT k=0;k<nh;k++)release(hc[k]);for(UINT k=0;k<nd;k++)release(dc[k]);}
};
// The BGRA pixel pass changes only compute UAV bindings. Keep their hidden
// counters and leave compute shader/SRVs/CBs/samplers untouched.
struct ComputeUavState {
 ID3D11DeviceContext*c;ID3D11UnorderedAccessView*uavs[8]{};
 explicit ComputeUavState(ID3D11DeviceContext*x):c(x){c->CSGetUnorderedAccessViews(0,8,uavs);ID3D11UnorderedAccessView*none[8]{};c->CSSetUnorderedAccessViews(0,8,none,nullptr);}
 ~ComputeUavState(){c->CSSetUnorderedAccessViews(0,8,uavs,nullptr);for(auto*p:uavs)if(p)p->Release();}
 ComputeUavState(const ComputeUavState&)=delete;ComputeUavState&operator=(const ComputeUavState&)=delete;
};
class D3D11ColorBlit {
 Microsoft::WRL::ComPtr<ID3D11VertexShader>vs_;Microsoft::WRL::ComPtr<ID3D11PixelShader>ps_;Microsoft::WRL::ComPtr<ID3D11RasterizerState>raster_;Microsoft::WRL::ComPtr<ID3D11Buffer>warpCb_;Microsoft::WRL::ComPtr<ID3D11SamplerState>sampler_;
 struct Samples {Microsoft::WRL::ComPtr<ID3D11Texture2D>source,fallback;Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>sourceView,fallbackView;};
 std::array<Samples,2>sampleViews_{};
 bool sampleView(ID3D11Device*d,ID3D11Texture2D*texture,Microsoft::WRL::ComPtr<ID3D11Texture2D>&identity,Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>&view){
  if(identity.Get()==texture&&view)return true;
  identity.Reset();view.Reset();if(!texture)return false;
  if(FAILED(d->CreateShaderResourceView(texture,nullptr,&view)))return false;identity=texture;return true;
 }
public:
 struct Batch {ID3D11DeviceContext*context;ComputeUavState compute;DrawState draw;explicit Batch(ID3D11DeviceContext*c):context(c),compute(c),draw(c){};Batch(const Batch&)=delete;Batch&operator=(const Batch&)=delete;};
 void clearSourceViews(){sampleViews_={};}
 std::unique_ptr<Batch>beginBatch(ID3D11DeviceContext*c){return c?std::make_unique<Batch>(c):nullptr;}
 bool initialize(ID3D11Device*d){const char*vs="float4 main(uint i:SV_VertexID):SV_Position{return float4(i==2?3:-1,i==1?3:-1,0,1);}";const char*ps=R"(Texture2D<float4>src:register(t0);Texture2D<float4>realImage:register(t1);SamplerState linearClamp:register(s0);cbuffer Warp:register(b0){float4 row0,row1,row2,tanFov,config;}
float4 main(float4 p:SV_Position):SV_Target{if(config.z<.5)return src.Load(int3((int2)p.xy,0));float2 uv=p.xy*config.xy;float3 ray=float3(lerp(tanFov.x,tanFov.y,uv.x),lerp(tanFov.z,tanFov.w,uv.y),-1);float3 oldRay=float3(dot(row0.xyz,ray),dot(row1.xyz,ray),dot(row2.xyz,ray));float2 oldUv=float2((oldRay.x/max(-oldRay.z,1e-8)-tanFov.x)/(tanFov.y-tanFov.x),(tanFov.z-oldRay.y/max(-oldRay.z,1e-8))/(tanFov.z-tanFov.w));if(oldRay.z>=-1e-8||any(oldUv<0)||any(oldUv>1))return realImage.SampleLevel(linearClamp,uv,0);return src.SampleLevel(linearClamp,oldUv,0);})";Microsoft::WRL::ComPtr<ID3DBlob>v,p,error;
 if(FAILED(D3DCompile(vs,strlen(vs),nullptr,nullptr,nullptr,"main","vs_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&v,&error))||FAILED(D3DCompile(ps,strlen(ps),nullptr,nullptr,nullptr,"main","ps_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&p,&error))||FAILED(d->CreateVertexShader(v->GetBufferPointer(),v->GetBufferSize(),nullptr,&vs_))||FAILED(d->CreatePixelShader(p->GetBufferPointer(),p->GetBufferSize(),nullptr,&ps_)))return false;D3D11_RASTERIZER_DESC r{};r.FillMode=D3D11_FILL_SOLID;r.CullMode=D3D11_CULL_NONE;r.DepthClipEnable=TRUE;if(FAILED(d->CreateRasterizerState(&r,&raster_)))return false;D3D11_BUFFER_DESC bd{};bd.ByteWidth=80;bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;if(FAILED(d->CreateBuffer(&bd,nullptr,&warpCb_)))return false;D3D11_SAMPLER_DESC sd{};sd.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;sd.AddressU=sd.AddressV=sd.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;sd.MaxLOD=3.402823466e+38F;return SUCCEEDED(d->CreateSamplerState(&sd,&sampler_));}
 bool copy(ID3D11Device*d,ID3D11DeviceContext*c,ID3D11Texture2D*source,ID3D11Texture2D*target,DXGI_FORMAT format,UINT w,UINT h,const RotationWarp*warp=nullptr,ID3D11Texture2D*fallback=nullptr,Batch*batch=nullptr,ID3D11RenderTargetView*cachedTarget=nullptr,unsigned eye=0){
 if(!d||!c||!source||!target||eye>=sampleViews_.size()||(batch&&batch->context!=c))return false;
 auto&samples=sampleViews_[eye];if(!sampleView(d,source,samples.source,samples.sourceView))return false;
 Microsoft::WRL::ComPtr<ID3D11RenderTargetView>rtv;if(cachedTarget)rtv=cachedTarget;else{D3D11_RENDER_TARGET_VIEW_DESC rd{};rd.Format=format;rd.ViewDimension=D3D11_RTV_DIMENSION_TEXTURE2D;if(FAILED(d->CreateRenderTargetView(target,&rd,&rtv)))return false;}
 if(warp&&!sampleView(d,fallback,samples.fallback,samples.fallbackView))return false;
 if(!warp){samples.fallbackView.Reset();samples.fallback.Reset();}
 std::unique_ptr<Batch>owned;if(!batch){owned=beginBatch(c);batch=owned.get();}c->SetPredication(nullptr,FALSE);auto*rt=rtv.Get();c->OMSetRenderTargets(1,&rt,nullptr);c->OMSetBlendState(nullptr,nullptr,UINT(-1));c->OMSetDepthStencilState(nullptr,0);c->RSSetState(raster_.Get());D3D11_VIEWPORT vp{0,0,(FLOAT)w,(FLOAT)h,0,1};c->RSSetViewports(1,&vp);c->IASetInputLayout(nullptr);c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);c->VSSetShader(vs_.Get(),nullptr,0);c->GSSetShader(nullptr,nullptr,0);c->HSSetShader(nullptr,nullptr,0);c->DSSetShader(nullptr,nullptr,0);c->PSSetShader(ps_.Get(),nullptr,0);struct Config{RotationWarp warp;float config[4];}cfg{};if(warp)cfg.warp=*warp;cfg.config[0]=1.f/w;cfg.config[1]=1.f/h;cfg.config[2]=warp?1.f:0.f;c->UpdateSubresource(warpCb_.Get(),0,nullptr,&cfg,0,0);auto*buffer=warpCb_.Get();auto*sampler=sampler_.Get();c->PSSetConstantBuffers(0,1,&buffer);c->PSSetSamplers(0,1,&sampler);ID3D11ShaderResourceView*views[2]={samples.sourceView.Get(),warp?samples.fallbackView.Get():nullptr};c->PSSetShaderResources(0,2,views);c->Draw(3,0);return true;}
};
}
