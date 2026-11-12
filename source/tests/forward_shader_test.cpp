#include "amd/stock_camera_d11.hpp"
#include "stock_ofxr_shader.hpp"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {
constexpr UINT extent = 128;
using Color = std::array<uint8_t, 4>;

void check(HRESULT result, const char* operation) {
    if (FAILED(result)) {
        std::fprintf(stderr, "%s failed (0x%08lx)\n", operation, static_cast<unsigned long>(result));
        throw std::runtime_error(operation);
    }
}

D3D11_TEXTURE2D_DESC textureDesc(UINT width, UINT height, DXGI_FORMAT format, UINT bindings) {
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
    desc.Format = format;
    desc.BindFlags = bindings;
    return desc;
}

ComPtr<ID3D11ShaderResourceView> inputView(
    ID3D11Device* device, UINT width, UINT height, DXGI_FORMAT format,
    const void* pixels, UINT rowBytes, D3D11_SRV_DIMENSION dimension) {
    const auto desc = textureDesc(width, height, format, D3D11_BIND_SHADER_RESOURCE);
    const D3D11_SUBRESOURCE_DATA initial{pixels, rowBytes, 0};
    ComPtr<ID3D11Texture2D> texture;
    check(device->CreateTexture2D(&desc, &initial, &texture), "create input texture");

    D3D11_SHADER_RESOURCE_VIEW_DESC viewDesc{};
    viewDesc.Format = format;
    viewDesc.ViewDimension = dimension;
    if (dimension == D3D11_SRV_DIMENSION_TEXTURE2DARRAY) {
        viewDesc.Texture2DArray.MipLevels = viewDesc.Texture2DArray.ArraySize = 1;
    } else {
        viewDesc.Texture2D.MipLevels = 1;
    }
    ComPtr<ID3D11ShaderResourceView> view;
    check(device->CreateShaderResourceView(texture.Get(), &viewDesc, &view), "create input view");
    return view;
}

void expectPixel(const D3D11_MAPPED_SUBRESOURCE& map, UINT x, UINT y, const Color& expected) {
    const auto* row = static_cast<const uint8_t*>(map.pData) + y * map.RowPitch;
    for (size_t channel = 0; channel < expected.size(); ++channel) {
        const auto actual = row[x * sizeof(Color) + channel];
        if (std::abs(int(actual) - int(expected[channel])) > 1) {
            std::fprintf(stderr, "Pixel (%u,%u) channel %zu: got %u, expected %u\n",
                         x, y, channel, unsigned(actual), unsigned(expected[channel]));
            throw std::runtime_error("forward output pixel mismatch");
        }
    }
}

void test() {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                           D3D11_SDK_VERSION, &device, nullptr, &context), "create D3D11 device");

    ofxr_color_d11::Composer composer;
    std::string diagnostics;
    const auto shader = composer.initialize(device.Get(), stock_ofxr_shader, diagnostics, true);
    if (FAILED(shader)) std::fputs(diagnostics.c_str(), stderr);
    check(shader, "compile forward shader");

    ofxr_color_d11::Parameters parameters;
    if (!ofxr_color_d11::set_packed_extent(parameters, extent, extent, 1, 2)) {
        throw std::runtime_error("set half-resolution flow extent");
    }
    parameters.flow_width = parameters.packed_width / parameters.flow_block_size;
    parameters.flow_height = parameters.packed_height / parameters.flow_block_size;
    parameters.synthesis_flags = 2u | ofxr_color_d11::fraction_flags(.5f);

    ofxr_color_d11::View pose;
    pose.fov = {-.8f, .8f, .7f, -.7f};
    pose.rect = {0, 0, extent, extent};
    if (!ofxr_color_d11::make_mapping(pose, pose, parameters.previous_mappings[0])) {
        throw std::runtime_error("create identity camera mapping");
    }
    parameters.previous_mappings[1] = parameters.previous_mappings[0];

    std::vector<Color> previous(extent * extent, Color{221, 222, 223, 255});
    std::vector<Color> current(extent * extent);
    for (UINT y = 0; y < extent; ++y) {
        for (UINT x = 0; x < extent; ++x) {
            current[y * extent + x] = {uint8_t(x), uint8_t(y), 201, 255};
        }
    }

    constexpr auto rgba = DXGI_FORMAT_R8G8B8A8_UNORM;
    constexpr auto colorArray = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
    constexpr auto plain = D3D11_SRV_DIMENSION_TEXTURE2D;
    const auto previousView = inputView(device.Get(), extent, extent, rgba, previous.data(), extent * sizeof(Color), colorArray);
    const auto currentView = inputView(device.Get(), extent, extent, rgba, current.data(), extent * sizeof(Color), colorArray);

    // B→A signed vectors are in packed pixels. At half resolution and
    // half an interval, output (x,y) samples B at (x−4,y+6).
    using Vector = std::array<int16_t, 2>;
    std::vector<Vector> vectors(parameters.flow_width * parameters.flow_height, Vector{-4, 6});
    const auto flow = inputView(device.Get(), parameters.flow_width, parameters.flow_height,
                                DXGI_FORMAT_R16G16_SINT, vectors.data(), parameters.flow_width * sizeof(Vector), plain);

    const auto outputDesc = textureDesc(extent, extent, rgba, D3D11_BIND_RENDER_TARGET);
    ComPtr<ID3D11Texture2D> output;
    ComPtr<ID3D11RenderTargetView> target;
    check(device->CreateTexture2D(&outputDesc, nullptr, &output), "create output texture");
    check(device->CreateRenderTargetView(output.Get(), nullptr, &target), "create output target");

    auto stagingDesc = textureDesc(extent, extent, rgba, 0);
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    check(device->CreateTexture2D(&stagingDesc, nullptr, &staging), "create readback texture");

    const D3D11_VIEWPORT viewport{0, 0, extent, extent, 0, 1};
    for (uint32_t sceneChange : {0u, 1u}) {
        // FidelityFX puts the recent scene change bits in texel 1.
        const uint32_t scene[3]{0, sceneChange, 0};
        const auto auxiliary = inputView(device.Get(), 3, 1, DXGI_FORMAT_R32_UINT, scene, sizeof(scene), plain);
        check(composer.synthesize(context.Get(), previousView.Get(), currentView.Get(),
                                  flow.Get(), auxiliary.Get(), target.Get(), parameters, viewport), "draw forward synthesis");

        context->CopyResource(staging.Get(), output.Get());
        D3D11_MAPPED_SUBRESOURCE map{};
        check(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &map), "read back forward output");
        expectPixel(map, 64, 64, sceneChange ? Color{64, 64, 201, 255} : Color{60, 70, 201, 255});
        expectPixel(map, 1, 64, Color{1, 64, 201, 255});
        context->Unmap(staging.Get(), 0);
    }
}
}

int main() {
    try {
        test();
        std::puts("PASS: forward B pixels move in the expected direction; edges and scene cuts retain current B");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
