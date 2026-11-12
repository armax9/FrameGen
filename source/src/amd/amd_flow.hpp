#pragma once
#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <FidelityFX/host/ffx_opticalflow.h>
#include <FidelityFX/host/backends/dx11/ffx_dx11.h>
#include <vector>
#include <mutex>
namespace actual_amd {
// The SDK uses process-global optical-flow constants. Serialize complete
// dispatch transactions across contexts/eyes, including priming and teardown.
class Flow {
public:
    ~Flow();
    FfxErrorCode initialize(ID3D11Device*,unsigned width,unsigned height);
    FfxErrorCode dispatch(ID3D11DeviceContext*,ID3D11Texture2D* color,bool reset);
    // Prepare an aligned previous A whose flow output is deliberately unused.
    // Final B must still use dispatch(); public SDK behavior remains unchanged.
    FfxErrorCode prepare_history(ID3D11DeviceContext*,ID3D11Texture2D* color,bool reset);
    ID3D11ShaderResourceView* vectors() const { return flow_srv_.Get(); }
    ID3D11ShaderResourceView* scene_change() const { return scd_srv_.Get(); }
    ID3D11Texture2D* vector_texture() const { return flow_.Get(); }
    unsigned width() const {return width_;}
    unsigned height() const {return height_;}
private:
    FfxErrorCode dispatch_impl(ID3D11DeviceContext*,ID3D11Texture2D*,bool reset,bool historyOnly);
    static std::mutex mutex_;
    FfxOpticalflowContext context_{};
    std::vector<char> scratch_;
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> flow_,scd_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> flow_srv_,scd_srv_;
    unsigned width_{},height_{};
    bool initialized_{};
};
}
