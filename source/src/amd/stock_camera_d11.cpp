#include "stock_camera_d11.hpp"
#include <d3dcompiler.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
using Microsoft::WRL::ComPtr;
namespace ofxr_color_d11 {
namespace {
constexpr char midpoint_shader[]=R"HLSL(
float4 SynthesizeAmdAlignedPS(FullscreenVertex input) : SV_Target {
    return synthesize_midpoint(input,1.0,false,false,false,false,true);
}
)HLSL";

constexpr char forward_shader[]=R"HLSL(
float4 SynthesizeAmdForwardPS(FullscreenVertex input) : SV_Target {
    uint2 pixel = uint2(input.position.xy);
    if (pixel.x >= Width || pixel.y >= Height || Slice >= ArraySize) {
        return float4(0.0, 0.0, 0.0, 1.0);
    }

    float4 current = CurrentFrame.Load(int4(int2(pixel), int(Slice), 0));
    if (repeated_capture_flag() != 0 ||
        (FlowAuxiliary.Load(int3(1, 0, 0)) & 0x0fU) != 0) {
        return saturate(current);
    }

    // Aligned AMD flow is the vector from B to A in B's camera. A point
    // moving forward by phase projects at B - phase * flow(B). Estimate
    // the inverse from the output pixel and sample only the current image.
    float2 backward = flow_for_pixel(float2(pixel), Slice, ViewIndex, 1.0, false);
    float2 source = float2(pixel) + synthesis_fraction() * backward;
    if (!coordinate_inside_rect(source, PreviousMappings[ViewIndex].TargetRect) ||
        map_target_to_source(source, PreviousMappings[ViewIndex]).valid < 0.5) {
        return saturate(current);
    }
    return saturate(bilinear_current_source(source, Slice));
}
)HLSL";

constexpr char repeat_shader[]=R"HLSL(
float4 RepeatCurrentPS(FullscreenVertex input) : SV_Target {
    return saturate(CurrentFrame.Load(int4(int2(input.position.xy), int(Slice), 0)));
}
)HLSL";

HRESULT compile(const std::string&s,const char*entry,const char*target,ID3DBlob**code,std::string&log){ComPtr<ID3DBlob>err;auto hr=D3DCompile(s.data(),s.size(),"OFXR-frame_synthesis-unmodified",nullptr,nullptr,entry,target,D3DCOMPILE_OPTIMIZATION_LEVEL3|D3DCOMPILE_ENABLE_STRICTNESS,0,code,&err);if(err)log.append(static_cast<const char*>(err->GetBufferPointer()),err->GetBufferSize());return hr;}
}
HRESULT Composer::initialize(ID3D11Device*d,const std::string&s,std::string&log,bool forward,bool repeat){if(!d||s.empty()||device_)return E_INVALIDARG;ComPtr<ID3DBlob>b;HRESULT hr=compile(s,"PackNvidiaFlowInput","cs_5_0",&b,log);if(FAILED(hr))return hr;hr=d->CreateComputeShader(b->GetBufferPointer(),b->GetBufferSize(),nullptr,&pack_);if(FAILED(hr))return hr;b.Reset();hr=compile(s,"FullscreenTriangleVS","vs_5_0",&b,log);if(FAILED(hr))return hr;hr=d->CreateVertexShader(b->GetBufferPointer(),b->GetBufferSize(),nullptr,&vertex_);if(FAILED(hr))return hr;b.Reset();
    const auto* source = repeat ? repeat_shader : forward ? forward_shader : midpoint_shader;
    const auto* entry = repeat ? "RepeatCurrentPS" : forward ? "SynthesizeAmdForwardPS" : "SynthesizeAmdAlignedPS";
    hr=compile(s+source,entry,"ps_5_0",&b,log);if(FAILED(hr))return hr;hr=d->CreatePixelShader(b->GetBufferPointer(),b->GetBufferSize(),nullptr,&pixel_);if(FAILED(hr))return hr;D3D11_BUFFER_DESC cb{};cb.ByteWidth=sizeof(Parameters);cb.Usage=D3D11_USAGE_DEFAULT;cb.BindFlags=D3D11_BIND_CONSTANT_BUFFER;hr=d->CreateBuffer(&cb,nullptr,&constants_);if(FAILED(hr))return hr;D3D11_RASTERIZER_DESC rs{};rs.FillMode=D3D11_FILL_SOLID;rs.CullMode=D3D11_CULL_NONE;rs.DepthClipEnable=TRUE;hr=d->CreateRasterizerState(&rs,&raster_);if(FAILED(hr))return hr;D3D11_DEPTH_STENCIL_DESC ds{};ds.DepthEnable=FALSE;ds.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ZERO;hr=d->CreateDepthStencilState(&ds,&depth_);if(FAILED(hr))return hr;D3D11_BLEND_DESC bs{};bs.RenderTarget[0].RenderTargetWriteMask=D3D11_COLOR_WRITE_ENABLE_ALL;hr=d->CreateBlendState(&bs,&blend_);if(FAILED(hr))return hr;repeat_=repeat;device_=d;return S_OK;}
HRESULT Composer::update(ID3D11DeviceContext*c,const Parameters&p){if(!device_||!c||c->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE||!p.width||!p.height||!p.array_size||p.array_size>2||!p.packed_width||!p.packed_height||p.flow_block_size!=8)return E_INVALIDARG;ComPtr<ID3D11Device>d;c->GetDevice(&d);if(d.Get()!=device_.Get())return E_INVALIDARG;c->UpdateSubresource(constants_.Get(),0,nullptr,&p,0,0);return S_OK;}
HRESULT Composer::packed_resources(const Parameters&p){if(p.packed_width==pw_&&p.packed_height==ph_&&packed_)return S_OK;packed_uav_.Reset();packed_srv_.Reset();packed_.Reset();previous_packed_uav_.Reset();previous_packed_.Reset();pw_=ph_=0;D3D11_TEXTURE2D_DESC td{};td.Width=p.packed_width;td.Height=p.packed_height;td.MipLevels=td.ArraySize=td.SampleDesc.Count=1;td.Format=DXGI_FORMAT_R8G8B8A8_UNORM;td.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS;auto hr=device_->CreateTexture2D(&td,nullptr,&packed_);if(FAILED(hr))return hr;hr=device_->CreateUnorderedAccessView(packed_.Get(),nullptr,&packed_uav_);if(FAILED(hr))return hr;hr=device_->CreateShaderResourceView(packed_.Get(),nullptr,&packed_srv_);if(FAILED(hr))return hr;hr=device_->CreateTexture2D(&td,nullptr,&previous_packed_);if(FAILED(hr))return hr;hr=device_->CreateUnorderedAccessView(previous_packed_.Get(),nullptr,&previous_packed_uav_);if(FAILED(hr))return hr;pw_=p.packed_width;ph_=p.packed_height;return S_OK;}
HRESULT Composer::pack_aligned(ID3D11DeviceContext*c,ID3D11ShaderResourceView*previous,ID3D11ShaderResourceView*current,const Parameters&input){
 if(!previous||!current)return E_POINTER;
 if(input.array_size!=1||input.slice!=0||input.view_index!=0)return E_INVALIDARG;
 Parameters p=input;p.synthesis_flags=(p.synthesis_flags|4u)&~16u;
 auto hr=update(c,p);if(FAILED(hr))return hr;hr=packed_resources(p);if(FAILED(hr))return hr;
 c->OMSetRenderTargets(0,nullptr,nullptr);
 ID3D11ShaderResourceView*inputs[8]={previous,current,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr};
 // Previous AMD pass may have left a flow UAV bound. This private context
 // transaction explicitly starts with no UAVs, then exposes stock u1/u2 only.
 ID3D11UnorderedAccessView*nulls[8]{};c->CSSetUnorderedAccessViews(0,8,nulls,nullptr);
 c->CSSetShaderResources(0,8,inputs);auto cb=constants_.Get();c->CSSetConstantBuffers(0,1,&cb);
 ID3D11UnorderedAccessView*outputs[2]={previous_packed_uav_.Get(),packed_uav_.Get()};
 c->CSSetUnorderedAccessViews(1,2,outputs,nullptr);c->CSSetShader(pack_.Get(),nullptr,0);
 c->Dispatch((p.packed_width+7)/8,(p.packed_height+7)/8,1);
 c->CSSetUnorderedAccessViews(0,8,nulls,nullptr);std::fill(std::begin(inputs),std::end(inputs),nullptr);c->CSSetShaderResources(0,8,inputs);c->CSSetShader(nullptr,nullptr,0);return S_OK;
}
HRESULT Composer::synthesize(ID3D11DeviceContext*c,ID3D11ShaderResourceView*previous,ID3D11ShaderResourceView*current,ID3D11ShaderResourceView*flow,ID3D11ShaderResourceView*scene,ID3D11RenderTargetView*target,const Parameters&p,const D3D11_VIEWPORT&vp){if(!previous||!current||!target||(!repeat_&&(!flow||!scene)))return E_POINTER;if(p.array_size!=1||p.view_index!=0||p.slice!=0)return E_INVALIDARG;if(!p.flow_width||!p.flow_height||p.view_index>1||p.slice>=p.array_size||vp.Width<=0||vp.Height<=0)return E_INVALIDARG;auto hr=update(c,p);if(FAILED(hr))return hr;ID3D11ShaderResourceView*inputs[8]={previous,current,flow,scene,nullptr,nullptr,nullptr,nullptr};auto cb=constants_.Get();c->CSSetShader(nullptr,nullptr,0);c->VSSetShader(vertex_.Get(),nullptr,0);c->HSSetShader(nullptr,nullptr,0);c->DSSetShader(nullptr,nullptr,0);c->GSSetShader(nullptr,nullptr,0);c->PSSetShader(pixel_.Get(),nullptr,0);c->VSSetConstantBuffers(0,1,&cb);c->PSSetConstantBuffers(0,1,&cb);c->PSSetShaderResources(0,8,inputs);c->IASetInputLayout(nullptr);c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);c->RSSetState(raster_.Get());c->RSSetViewports(1,&vp);c->OMSetDepthStencilState(depth_.Get(),0);const float factors[4]{};c->OMSetBlendState(blend_.Get(),factors,0xffffffff);c->OMSetRenderTargets(1,&target,nullptr);c->Draw(3,0);std::fill(std::begin(inputs),std::end(inputs),nullptr);c->PSSetShaderResources(0,8,inputs);c->OMSetRenderTargets(0,nullptr,nullptr);return S_OK;}
}
