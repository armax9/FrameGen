#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <thread>

using Microsoft::WRL::ComPtr;
using Clock = std::chrono::steady_clock;
using namespace std::chrono;
static void require(bool ok) { if (!ok) std::exit(1); }

int main() {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    require(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context)));
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = 2368; desc.Height = 2560;
    desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> source[2], staging[2][2];
    ComPtr<ID3D11RenderTargetView> target[2];
    for (unsigned eye = 0; eye < 2; ++eye) {
        require(SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &source[eye])));
        require(SUCCEEDED(device->CreateRenderTargetView(source[eye].Get(), nullptr, &target[eye])));
        auto cpu = desc;
        cpu.Usage = D3D11_USAGE_STAGING;
        cpu.BindFlags = 0; cpu.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        for (auto& slot : staging[eye])
            require(SUCCEEDED(device->CreateTexture2D(&cpu, nullptr, &slot)));
    }
    std::vector<unsigned char> publication[2];
    for (auto& pixels : publication) pixels.resize(size_t(desc.Width) * desc.Height * 4);
    auto (*volatile copy)(void*, const void*, size_t) = std::memcpy;
    std::printf("rate,policy,frame,tag,published,age_us,map_us,publication_us,slot_late\n");
    for (unsigned rate : {72u, 90u}) {
        for (unsigned policy : {0u, 1u, 2u, 2u, 1u, 0u}) {
            const auto period = nanoseconds(1000000000 / rate);
            auto next = Clock::now();
            Clock::time_point timestamp[2]{};
            for (unsigned frame = 1; frame <= 96; ++frame) {
                std::this_thread::sleep_until(next);
                const auto slot = next;
                next += period;
                const unsigned write = frame % 2;
                timestamp[write] = Clock::now();
                const unsigned expected = 1 + frame % 250;
                const float color[] = {expected / 255.f, 0.f, 0.f, 1.f};
                for (unsigned eye = 0; eye < 2; ++eye) {
                    context->ClearRenderTargetView(target[eye].Get(), color);
                    context->CopyResource(staging[eye][write].Get(), source[eye].Get());
                }
                context->Flush();
                if (frame == 1) continue;
                unsigned read = policy == 0 ? 1 - write : write;
                auto mapAt = Clock::now();
                D3D11_MAPPED_SUBRESOURCE mapped[2]{};
                auto mapPair = [&](unsigned flags) {
                    unsigned eye = 0;
                    for (; eye < 2; ++eye) {
                        const auto result = context->Map(staging[eye][read].Get(), 0, D3D11_MAP_READ, flags, &mapped[eye]);
                        if (result == DXGI_ERROR_WAS_STILL_DRAWING && flags) break;
                        require(SUCCEEDED(result));
                    }
                    if (eye == 2) return true;
                    while (eye) context->Unmap(staging[--eye][read].Get(), 0);
                    return false;
                };
                bool ready = mapPair(policy == 2 ? D3D11_MAP_FLAG_DO_NOT_WAIT : 0);
                if (!ready) {
                    read = 1 - write;
                    ready = mapPair(D3D11_MAP_FLAG_DO_NOT_WAIT);
                }
                const auto mapDone = Clock::now();
                const unsigned tag = frame - (read != write);
                if (ready) {
                    for (unsigned eye = 0; eye < 2; ++eye) {
                        const auto* pixels = static_cast<const unsigned char*>(mapped[eye].pData);
                        const size_t rowBytes = size_t(desc.Width) * 4;
                        for (unsigned row = 0; row < desc.Height; ++row)
                            copy(publication[eye].data() + row * rowBytes, pixels + row * mapped[eye].RowPitch, rowBytes);
                        require(publication[eye].front() == 1 + tag % 250);
                        require(publication[eye][publication[eye].size() - 4] == 1 + tag % 250);
                        context->Unmap(staging[eye][read].Get(), 0);
                    }
                }
                const auto publishAt = Clock::now();
                std::printf("%u,%u,%u,%u,%u,%lld,%lld,%lld,%u\n", rate, policy, frame, tag,
                    unsigned(ready), (long long)duration_cast<microseconds>(publishAt - timestamp[read]).count(),
                    (long long)duration_cast<microseconds>(mapDone - mapAt).count(),
                    (long long)duration_cast<microseconds>(publishAt.time_since_epoch()).count(),
                    unsigned(publishAt - slot >= period));
            }
        }
    }
}
