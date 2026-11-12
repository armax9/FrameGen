#pragma once
#include <d3d11_1.h>
#include <wrl/client.h>

// Setting a buffer through the D3D11.0 API resets its D3D11.1 binding range.
// Preserve both parts of the binding whenever the context supports ranges.
template<UINT Count,auto Get,auto Set,auto GetRange,auto SetRange>
class ConstantBufferBindings {
 ID3D11DeviceContext* context_;
 Microsoft::WRL::ComPtr<ID3D11DeviceContext1> context1_;
 ID3D11Buffer* buffers_[Count]{};
 UINT first_[Count]{},count_[Count]{};
public:
 explicit ConstantBufferBindings(ID3D11DeviceContext* context):context_(context){
  if(SUCCEEDED(context->QueryInterface(IID_PPV_ARGS(&context1_))))
   (context1_.Get()->*GetRange)(0,Count,buffers_,first_,count_);
  else
   (context_->*Get)(0,Count,buffers_);
 }
 ~ConstantBufferBindings(){for(auto* buffer:buffers_)if(buffer)buffer->Release();}
 ConstantBufferBindings(const ConstantBufferBindings&)=delete;
 ConstantBufferBindings& operator=(const ConstantBufferBindings&)=delete;
 void restore()const{
  if(context1_)
   (context1_.Get()->*SetRange)(0,Count,buffers_,first_,count_);
  else
   (context_->*Set)(0,Count,buffers_);
 }
};

template<UINT Count> using PixelConstantBuffers=ConstantBufferBindings<Count,
 &ID3D11DeviceContext::PSGetConstantBuffers,&ID3D11DeviceContext::PSSetConstantBuffers,
 &ID3D11DeviceContext1::PSGetConstantBuffers1,&ID3D11DeviceContext1::PSSetConstantBuffers1>;
template<UINT Count> using VertexConstantBuffers=ConstantBufferBindings<Count,
 &ID3D11DeviceContext::VSGetConstantBuffers,&ID3D11DeviceContext::VSSetConstantBuffers,
 &ID3D11DeviceContext1::VSGetConstantBuffers1,&ID3D11DeviceContext1::VSSetConstantBuffers1>;
template<UINT Count> using ComputeConstantBuffers=ConstantBufferBindings<Count,
 &ID3D11DeviceContext::CSGetConstantBuffers,&ID3D11DeviceContext::CSSetConstantBuffers,
 &ID3D11DeviceContext1::CSGetConstantBuffers1,&ID3D11DeviceContext1::CSSetConstantBuffers1>;

struct ComputeState {
 ID3D11DeviceContext*c;
 ID3D11ComputeShader*shader=nullptr;ID3D11ClassInstance*classes[256]{};UINT n=256;
 ID3D11ShaderResourceView*srvs[128]{};ID3D11UnorderedAccessView*uavs[8]{};
 ComputeConstantBuffers<14> constants;ID3D11SamplerState*samplers[16]{};
 ComputeState(ID3D11DeviceContext*ctx):c(ctx),constants(ctx){c->CSGetShader(&shader,classes,&n);c->CSGetShaderResources(0,128,srvs);c->CSGetUnorderedAccessViews(0,8,uavs);c->CSGetSamplers(0,16,samplers);}
 ~ComputeState(){ID3D11UnorderedAccessView*zero[8]{};ID3D11ShaderResourceView*empty[128]{};c->CSSetUnorderedAccessViews(0,8,zero,nullptr);c->CSSetShaderResources(0,128,empty);c->CSSetShader(shader,classes,n);constants.restore();c->CSSetShaderResources(0,128,srvs);c->CSSetSamplers(0,16,samplers);c->CSSetUnorderedAccessViews(0,8,uavs,nullptr);if(shader)shader->Release();for(UINT i=0;i<n;i++)if(classes[i])classes[i]->Release();for(auto*p:srvs)if(p)p->Release();for(auto*p:uavs)if(p)p->Release();for(auto*p:samplers)if(p)p->Release();}
};

// Only for Pipeline::prepare, whose shaders/bindings are fully controlled here.
// Do not use around opaque NGX evaluation (which retains the full guard above).
struct PreparationState {
 ID3D11DeviceContext*c;ID3D11ComputeShader*shader=nullptr;ID3D11ClassInstance*classes[256]{};UINT n=256;
 ID3D11ShaderResourceView*srvs[2]{};ID3D11UnorderedAccessView*uavs[8]{};ComputeConstantBuffers<1> constants;
 explicit PreparationState(ID3D11DeviceContext*x):c(x),constants(x){c->CSGetShader(&shader,classes,&n);c->CSGetShaderResources(0,2,srvs);c->CSGetUnorderedAccessViews(0,8,uavs);}
 ~PreparationState(){ID3D11UnorderedAccessView*none[8]{};ID3D11ShaderResourceView*empty[2]{};c->CSSetUnorderedAccessViews(0,8,none,nullptr);c->CSSetShaderResources(0,2,empty);c->CSSetShader(shader,classes,n);constants.restore();c->CSSetShaderResources(0,2,srvs);c->CSSetUnorderedAccessViews(0,8,uavs,nullptr);if(shader)shader->Release();for(UINT i=0;i<n;i++)if(classes[i])classes[i]->Release();for(auto*p:srvs)if(p)p->Release();for(auto*p:uavs)if(p)p->Release();}
};

