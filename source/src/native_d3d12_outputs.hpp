#pragma once

#include "native_frame_path.hpp"

namespace runtime_study {

// Use on the generation worker. The generator retains textures through its
// GPU fence. Stock Molten capture needs the source resource layout unchanged.
class NativeD3D12Outputs final {
public:
    struct EyeDescription {
        std::uint32_t width{}, height{};
        DXGI_FORMAT format{DXGI_FORMAT_UNKNOWN};

        bool operator==(const EyeDescription&) const = default;
    };

    explicit NativeD3D12Outputs(NativeFramePath& endpoint) : endpoint_(endpoint) {}
    NativeD3D12Outputs(const NativeD3D12Outputs&) = delete;
    NativeD3D12Outputs& operator=(const NativeD3D12Outputs&) = delete;

    // Destroy after the generator's writes have retired and before the
    // endpoint is reset. The endpoint handles remaining native consumers.
    ~NativeD3D12Outputs() {
        for (const auto& chain : chains_)
            if (chain.handle) endpoint_.destroy_swapchain(chain.handle);
    }

    [[nodiscard]] std::optional<NativeFramePath::D3D12OutputImages> acquire(
        const std::array<EyeDescription, 2>& descriptions) {
        return acquire(descriptions, nullptr);
    }

    [[nodiscard]] std::optional<NativeFramePath::D3D12OutputImages> acquire(
        const std::array<EyeDescription, 2>& descriptions,
        const std::array<XrSwapchainSubImage, 2>& sourceSubImages) {
        return acquire(descriptions, &sourceSubImages);
    }

private:
    struct Chain {
        XrSwapchain handle{};
        XrSwapchain source{};
        EyeDescription description{};
        bool unsupported{};
    };

    [[nodiscard]] std::optional<NativeFramePath::D3D12OutputImages> acquire(
        const std::array<EyeDescription, 2>& descriptions,
        const std::array<XrSwapchainSubImage, 2>* sourceSubImages) {
        std::array<XrSwapchain, 2> sources{};
        for (std::size_t eye = 0; eye < descriptions.size(); ++eye) {
            const auto& desc = descriptions[eye];
            if (!desc.width || !desc.height || desc.width > INT32_MAX || desc.height > INT32_MAX ||
                desc.format == DXGI_FORMAT_UNKNOWN)
                return std::nullopt;
            if (!sourceSubImages) continue;
            const auto& subImage = (*sourceSubImages)[eye];
            if (!subImage.swapchain || subImage.imageRect.extent.width != int32_t(desc.width) ||
                subImage.imageRect.extent.height != int32_t(desc.height))
                return std::nullopt;
            sources[eye] = subImage.swapchain;
        }
        const bool shared = sources[0] && sources[0] == sources[1];
        if (shared && descriptions[0].format != descriptions[1].format) return std::nullopt;
        const std::size_t chainCount = shared ? 1 : chains_.size();

        for (std::size_t eye = 0; eye < chains_.size(); ++eye) {
            auto& chain = chains_[eye];
            const auto source = sources[eye];
            if (chain.unsupported) {
                if (eye < chainCount && chain.source == source && chain.description == descriptions[eye])
                    return std::nullopt;
                chain = {};
            }
            if (!chain.handle) continue;
            if (eye < chainCount && chain.source == source &&
                (source ? endpoint_.output_matches_source12(chain.handle, source)
                        : chain.description == descriptions[eye]))
                continue;
            // Retire old rings without waiting on the generation worker.
            if (endpoint_.try_destroy_output_swapchain12(chain.handle) != XR_SUCCESS) return std::nullopt;
            chain = {};
        }
        for (std::size_t eye = 0; eye < chainCount; ++eye) {
            auto& chain = chains_[eye];
            if (chain.handle) continue;
            const auto& desc = descriptions[eye];
            const auto source = sources[eye];
            XrSwapchain created{};
            const auto result = source ? endpoint_.create_matching_output_swapchain12(source, &created)
                : endpoint_.create_output_swapchain12(desc.width, desc.height, desc.format, &created);
            if (result != XR_SUCCESS) {
                // A stock runtime may never return a matching descriptor for
                // this source. Avoid creating and destroying a chain per frame.
                if (source && result == XR_ERROR_FEATURE_UNSUPPORTED) chain = {{}, source, desc, true};
                return std::nullopt;
            }
            chain = {created, source, desc};
        }

        std::array<XrSwapchainSubImage, 2> outputs{};
        for (std::size_t eye = 0; eye < outputs.size(); ++eye) {
            const auto& desc = descriptions[eye];
            outputs[eye] = sourceSubImages ? (*sourceSubImages)[eye]
                : XrSwapchainSubImage{XR_NULL_HANDLE, {{0, 0}, {int32_t(desc.width), int32_t(desc.height)}}, 0};
            outputs[eye].swapchain = chains_[shared ? 0 : eye].handle;
        }
        return endpoint_.try_acquire_output_images12(outputs);
    }

    NativeFramePath& endpoint_;
    std::array<Chain, 2> chains_{};
};

} // namespace runtime_study
