#pragma once

#include "molten_event_worker.hpp"
#include <openxr/openxr.h>
#include <array>
#include <cstdint>
#include <cstring>

namespace runtime_study {

// Private state layout of the pinned Oct 26 MoltenVR Windows runtime. The
// launcher verifies the full DLL hash. In-process witnesses bind these reads
// to that runtime's actual xrEndFrame, rather than an intervening API layer.
class MoltenEventRuntime {
    static constexpr std::size_t endFrameRva = 0x196b0;
    static constexpr std::size_t tagRingRva = 0x661a0;
    static constexpr std::size_t shmRva = 0x662f0;
    unsigned char* image_{};
    MoltenEventWorker worker_;
    bool ownsPublication_{};
    std::atomic<std::uint64_t> staged_{}, fallback_{};

    template<class T> T read(std::size_t rva) const noexcept {
        T value{};
        std::memcpy(&value, image_ + rva, sizeof(value));
        return value;
    }

    bool cpuMode() const noexcept {
        return image_ && !read<unsigned char>(0x634c1) && !read<std::uint32_t>(0x63768);
    }

    std::uint64_t exactTag(XrTime time) const noexcept {
        struct Entry { XrTime time; std::uint64_t tag; };
        for (unsigned index = 0; index < 16; ++index) {
            const auto entry = read<Entry>(tagRingRva + index * sizeof(Entry));
            if (entry.time == time) return entry.tag;
        }
        return 0;
    }

public:
    MoltenEventRuntime(ID3D11Device* device, ID3D11DeviceContext* context,
                       xrfg::ContextEncodingGate* gate, bool earlyCurrent = false,
                       bool packedCurrent = false, bool mapFirst = false, bool bufferCurrent = false,
                       bool directMapped = false, uint64_t fenceWaitBudgetUs = 70'000)
        : worker_(device, context, gate, earlyCurrent, packedCurrent, mapFirst, bufferCurrent, directMapped,
                  fenceWaitBudgetUs) {}

    bool initialize(PFN_xrEndFrame nativeEnd) {
        HMODULE module{};
        if (!nativeEnd || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(nativeEnd), &module)) return false;
        auto* base = reinterpret_cast<unsigned char*>(module);
        if (reinterpret_cast<const void*>(nativeEnd) != base + endFrameRva) return false;

        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 || dos->e_lfanew > 4096) return false;
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE || nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
            nt->FileHeader.TimeDateStamp != 1793033881 || nt->OptionalHeader.SizeOfImage != 0x262000 ||
            nt->OptionalHeader.CheckSum != 0x2c8e90) return false;

        static constexpr unsigned char ringLookup[] = {0x48,0x8d,0x05,0x9a,0xc9,0x04,0x00,0x48,0x8b,0x55,0x10};
        static constexpr unsigned char stockReset[] = {0x66,0x44,0x89,0x15,0x66,0x88,0x04,0x00,0x66,0x44,0x89,0x1d,0xae,0x88,0x04,0x00};
        if (std::memcmp(base + 0x197ff, ringLookup, sizeof ringLookup) ||
            std::memcmp(base + 0x1af56, stockReset, sizeof stockReset) ||
            !worker_.initialize()) return false;
        image_ = base;
        return true;
    }

    // Called with both the native pose and encoding gates held. Images are
    // the presenter's waited native targets and have yet to be released.
    std::uint64_t stage(const XrFrameEndInfo& frame,
                        const std::array<ID3D11Texture2D*, 2>& eyes) {
        if (!cpuMode() || frame.environmentBlendMode != XR_ENVIRONMENT_BLEND_MODE_OPAQUE ||
            frame.next || frame.layerCount != 1 || !frame.layers) return 0;
        const auto* layer = frame.layers[0];
        if (!layer || layer->type != XR_TYPE_COMPOSITION_LAYER_PROJECTION || layer->next ||
            (layer->layerFlags & ~(XR_COMPOSITION_LAYER_CORRECT_CHROMATIC_ABERRATION_BIT |
                                  XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT))) return 0;
        const auto& projection = *reinterpret_cast<const XrCompositionLayerProjection*>(layer);
        if (projection.viewCount != 2 || !projection.views) return 0;

        const auto width = read<std::uint32_t>(0x662fc);
        const auto height = read<std::uint32_t>(0x662f8);
        auto* memory = read<unsigned char*>(shmRva);
        if (!memory || width < 128 || width > 4096 || height < 128 || height > 4096) return 0;
        MEMORY_BASIC_INFORMATION region{};
        const auto bytes = std::size_t{4096} + std::size_t{16} * width * height;
        if (!VirtualQuery(memory, &region, sizeof region) || region.State != MEM_COMMIT ||
            region.BaseAddress != memory || region.RegionSize < bytes ||
            !(region.Protect & (PAGE_READWRITE | PAGE_WRITECOPY))) return 0;

        auto* words = reinterpret_cast<volatile LONG*>(memory);
        if (InterlockedCompareExchange(words + 8, 0, 0) || InterlockedCompareExchange(words + 14, 0, 0))
            return 0;
        for (unsigned index = 0; index < 2; ++index) {
            const auto& view = projection.views[index];
            const auto& rect = view.subImage.imageRect;
            if (view.next || view.subImage.imageArrayIndex || rect.offset.x || rect.offset.y ||
                rect.extent.width != static_cast<int>(width) || rect.extent.height != static_cast<int>(height) ||
                !(view.fov.angleLeft < view.fov.angleRight && view.fov.angleDown < view.fov.angleUp))
                return 0;
        }
        const auto tag = exactTag(frame.displayTime);
        if (!tag) return 0;
        const auto token = worker_.stage(eyes, width, height, memory, tag);
        if (token) ++staged_;
        return token;
    }

    void finish(std::uint64_t token, bool accepted, std::uint64_t nativeEndUs) {
        // Empty End refreshes the runtime's manual flip state. If the user
        // changes it between staging and handoff, the next frame uses stock.
        accepted = accepted && cpuMode();
        if (accepted) ownsPublication_ = true;
        worker_.finish(token, accepted, nativeEndUs);
    }

    MoltenEventWorker::Preparation earlyPreparation(std::uint64_t token) noexcept {
        if (!token || !cpuMode()) return {};
        return worker_.earlyPreparation(token, read<unsigned char*>(shmRva),
                                        read<std::uint32_t>(0x662fc), read<std::uint32_t>(0x662f8));
    }

    bool publishEarly(std::uint64_t token) noexcept {
        if (!cpuMode() || !worker_.publishEarly(token, read<unsigned char*>(shmRva),
                read<std::uint32_t>(0x662fc), read<std::uint32_t>(0x662f8))) return false;
        ownsPublication_ = true;
        return true;
    }

    bool drain() { return worker_.drain(); }
    void shutdown() noexcept { worker_.shutdown(); }
    std::array<std::uint64_t,5> counters() const noexcept {
        return {staged_.load(),worker_.published_count(),worker_.failure_count(),fallback_.load(),worker_.expired_count()};
    }

    // Called with native gates held. The asynchronous publisher must drain
    // first. An early worker only writes to private scratch before commit.
    void resumeStock() {
        if (!ownsPublication_) return;
        ownsPublication_ = false;
        const std::uint16_t invalid{};
        const std::uint64_t zeroTag{};
        std::memcpy(image_ + 0x637c4, &invalid, sizeof invalid);
        std::memcpy(image_ + 0x63814, &invalid, sizeof invalid);
        std::memcpy(image_ + 0x63088, &zeroTag, sizeof zeroTag);
        image_[0x632d0] = 0; // stock also clears the quad-only base after an ordinary projection
        ++fallback_;
    }
};

} // namespace runtime_study
