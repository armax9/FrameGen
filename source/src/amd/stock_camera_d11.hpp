#pragma once
#include <d3d11.h>
#include "camera_contract.hpp"
#include <wrl/client.h>
#include <array>
#include <cstdint>
#include <string>

// Isolated aligned-AMD experiment: stock per-eye NVIDIA pack math + stock
// composition function selected with AMD pixel-unit flow and pose compensation.
// No scheduler, flow estimator, source lifetime or game hook is implemented here.
// Source: OFXR-Bridge d7fcca72d569ec364281ab63ae29378bbfaf1251 (GPL-3.0).
namespace ofxr_color_d11 {

class Composer {
public:
    // Calls D3DCompile then creates shaders/resources. Caller supplies exact
    // unmodified frame_synthesis.hlsl. No GPU work is submitted by this method.
    HRESULT initialize(ID3D11Device*,const std::string& shader,std::string& diagnostics,
                       bool forward=false,bool repeat=false);
    // Caller must own immediate-context state and retain every source through
    // GPU completion. Methods do not preserve the application's other bindings.
    // previous/current SRVs must be Texture2DArray even for array_size==1.
    // Aligned route is ONE logical eye per composer/context. Both source SRVs
    // must expose that eye as array slice0; Parameters.array_size=1,slice=0.
    HRESULT pack_aligned(ID3D11DeviceContext*,ID3D11ShaderResourceView* previous,
                         ID3D11ShaderResourceView* current,const Parameters&);
    HRESULT synthesize(ID3D11DeviceContext*,ID3D11ShaderResourceView* previous,
                       ID3D11ShaderResourceView* current,ID3D11ShaderResourceView* backward_flow,
                       ID3D11ShaderResourceView* scene_change,ID3D11RenderTargetView* target,
                       const Parameters&,const D3D11_VIEWPORT&);
    ID3D11Texture2D* packed_previous()const noexcept{return previous_packed_.Get();}
    ID3D11Texture2D* packed_color()const noexcept{return packed_.Get();}
    ID3D11ShaderResourceView* packed_srv()const noexcept{return packed_srv_.Get();}
private:
    HRESULT update(ID3D11DeviceContext*,const Parameters&);
    HRESULT packed_resources(const Parameters&);
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> pack_;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> vertex_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> pixel_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> constants_;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> raster_;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> depth_;
    Microsoft::WRL::ComPtr<ID3D11BlendState> blend_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> packed_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> packed_srv_;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> packed_uav_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> previous_packed_;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> previous_packed_uav_;
    uint32_t pw_{},ph_{};
    bool repeat_{};
};
}
