#include <d3d11_1.h>
#include "bgra_blit.hpp"
#include <cstdio>
using Microsoft::WRL::ComPtr;

int main() {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                                 D3D11_SDK_VERSION, &device, nullptr, &context))) return 1;
    ComPtr<ID3D11DeviceContext1> context1;
    if (FAILED(context.As(&context1))) { std::puts("No D3D11.1 context"); return 2; }
    D3D11_BUFFER_DESC desc{};
    desc.ByteWidth = 1024;
    desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    ComPtr<ID3D11Buffer> buffer;
    if (FAILED(device->CreateBuffer(&desc, nullptr, &buffer))) return 3;
    auto* ptr = buffer.Get();
    UINT first = 16, count = 16;
    context1->PSSetConstantBuffers1(0, 1, &ptr, &first, &count);
    auto report = [&](const char* label) {
        ComPtr<ID3D11Buffer> actual;
        UINT actualFirst = 0xffffffff, actualCount = 0xffffffff;
        context1->PSGetConstantBuffers1(0, 1, &actual, &actualFirst, &actualCount);
        std::printf("%s: buffer=%s first=%u count=%u\n", label,
            actual.Get() == ptr ? "same" : "different", actualFirst, actualCount);
        return actual.Get() == ptr && actualFirst == first && actualCount == count;
    };
    if (!report("Before FrameGen draw")) return 4;
    {
        presentation_study::DrawState saved(context.Get());
        ID3D11Buffer* none = nullptr;
        context->PSSetConstantBuffers(0, 1, &none);
    }
    return report("After FrameGen draw") ? 0 : 5;
}
