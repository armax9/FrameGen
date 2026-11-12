#pragma once

#include <windows.h>
#include <d3d11_4.h>
#include <wrl/client.h>
#include "xrfg/context_encoding_gate.hpp"
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <emmintrin.h>
#include <memory>
#include <mutex>
#include <new>
#include <thread>
#include <utility>
#include "molten_stereo_buffer_readback.hpp"

namespace runtime_study {

// The consumer reserves and submits while it owns the shared encoding gate.
// A separate worker owns CPU readback and publication. Drain before entering
// either native End gate when changing publishers, and before unmapping SHM.
class MoltenEventWorker {
public:
    struct Preparation {
        uint64_t finishedAt{}, fenceAt{}, mapUs{}, mapCallUs{}, mapGateUs{}, copyUs{};
        unsigned fenceTimeouts{}, mapRetries{};
    };

private:
    using Texture = Microsoft::WRL::ComPtr<ID3D11Texture2D>;
    struct Slot {
        std::array<Texture, 2> input, staging;
        MoltenStereoBufferReadback::Slot linear;
        std::array<bool, 2> bgra{};
        unsigned width{}, height{};
        unsigned char* shm{};
        const void* mappedPixels{};
        std::unique_ptr<unsigned char[]> pixels;
        size_t pixelCapacity{};
        uint64_t token{}, tag{}, endUs{}, stagedUs{};
        Preparation preparation;
        bool decided{}, accepted{}, prepared{};
    };

    static constexpr uint64_t minimumAgeUs = 10'000;
    static constexpr uint64_t maximumAgeUs = 26'000;

    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext4> context_;
    Microsoft::WRL::ComPtr<ID3D11Fence> fence_;
    xrfg::ContextEncodingGate* gate_{};
    HANDLE fenceEvent_{}, stopEvent_{}, timer_{};
    std::array<Slot, 3> slots_;
    std::mutex mutex_;
    std::condition_variable changed_;
    std::thread worker_;
    uint64_t issued_{}, retired_{};
    MoltenStereoBufferReadback linearReadback_;
    const bool earlyCurrent_, packedCurrent_, mapFirst_, bufferCurrent_, directMapped_;
    const uint64_t fenceWaitBudgetUs_;
    bool initialized_{}, stopping_{}, untrackedCopy_{};
    std::atomic<bool> failed_{};
    std::atomic<uint64_t> published_{}, failures_{}, expired_{};

    static uint64_t nowUs() noexcept {
        static const uint64_t frequency = [] {
            LARGE_INTEGER value{};
            QueryPerformanceFrequency(&value);
            return uint64_t(value.QuadPart);
        }();
        LARGE_INTEGER counter{};
        QueryPerformanceCounter(&counter);
        const auto ticks = uint64_t(counter.QuadPart);
        return ticks / frequency * 1'000'000 + ticks % frequency * 1'000'000 / frequency;
    }

    static uint32_t load32(unsigned char* p) noexcept {
        return uint32_t(InterlockedCompareExchange(reinterpret_cast<volatile LONG*>(p), 0, 0));
    }

    static uint64_t load64(unsigned char* p) noexcept {
        return uint64_t(InterlockedCompareExchange64(reinterpret_cast<volatile LONG64*>(p), 0, 0));
    }

    static void store32(unsigned char* p, uint32_t value) noexcept {
        InterlockedExchange(reinterpret_cast<volatile LONG*>(p), LONG(value));
    }

    static void store64(unsigned char* p, uint64_t value) noexcept {
        InterlockedExchange64(reinterpret_cast<volatile LONG64*>(p), LONG64(value));
    }

    Slot* find(uint64_t token) noexcept {
        for (auto& slot : slots_)
            if (slot.token == token) return &slot;
        return nullptr;
    }

    void fail() noexcept {
        failed_.store(true);
        failures_.fetch_add(1);
    }

    static bool describe(ID3D11Texture2D* texture, unsigned width, unsigned height,
                         D3D11_TEXTURE2D_DESC& desc, bool& bgra) noexcept {
        if (!texture) return false;
        texture->GetDesc(&desc);
        bgra = desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM ||
               desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        return desc.Width == width && desc.Height == height && desc.ArraySize == 1 &&
               desc.MipLevels == 1 && desc.SampleDesc.Count == 1 &&
               (bgra || desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM ||
                        desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
    }

    static bool matches(const Texture& staging, const D3D11_TEXTURE2D_DESC& desc) noexcept {
        if (!staging) return false;
        D3D11_TEXTURE2D_DESC current{};
        staging->GetDesc(&current);
        return current.Width == desc.Width && current.Height == desc.Height &&
               current.Format == desc.Format;
    }

    Slot* select(const std::array<D3D11_TEXTURE2D_DESC, 2>& desc) noexcept {
        Slot* unused{};
        Slot* spare{};
        for (auto& slot : slots_) {
            if (slot.token) continue;
            if (bufferCurrent_) return &slot;
            if (matches(slot.staging[0], desc[0]) && (packedCurrent_ || matches(slot.staging[1], desc[1])))
                return &slot;
            if (!unused && !slot.staging[0] && !slot.staging[1]) unused = &slot;
            if (!spare) spare = &slot;
        }
        return unused ? unused : spare;
    }

    bool prepare(Texture& staging, D3D11_TEXTURE2D_DESC desc) noexcept {
        if (matches(staging, desc)) return true;
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = desc.MiscFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        desc.SampleDesc.Quality = 0;
        Texture replacement;
        if (FAILED(device_->CreateTexture2D(&desc, nullptr, &replacement))) return false;
        staging = std::move(replacement);
        return true;
    }

    bool fenceReady(uint64_t token, unsigned* timeouts = nullptr) noexcept {
        const auto complete = fence_->GetCompletedValue();
        if (complete == UINT64_MAX) return false;
        if (complete >= token) return true;
        if (FAILED(fence_->SetEventOnCompletion(token, fenceEvent_))) return false;
        const HANDLE events[]{stopEvent_, fenceEvent_};
        // Recheck the serial on wake. A late event from an earlier aborted
        // request cannot validate the current pair.
        const auto until = nowUs() + fenceWaitBudgetUs_;
        while (nowUs() < until) {
            const auto event = WaitForMultipleObjects(2, events, FALSE, 5);
            if (event == WAIT_OBJECT_0) return false;
            if (timeouts && event == WAIT_TIMEOUT) ++*timeouts;
            const auto ready = fence_->GetCompletedValue();
            if (ready == UINT64_MAX) return false;
            if (ready >= token) return true;
        }
        return false;
    }

    bool waitUntil(uint64_t target) noexcept {
        const auto now = nowUs();
        if (target <= now) return WaitForSingleObject(stopEvent_, 0) == WAIT_TIMEOUT;
        LARGE_INTEGER due{};
        due.QuadPart = -static_cast<LONGLONG>((target - now) * 10);
        if (!SetWaitableTimer(timer_, &due, 0, nullptr, nullptr, FALSE)) return false;
        const HANDLE events[]{stopEvent_, timer_};
        return WaitForMultipleObjects(2, events, FALSE, 30) == WAIT_OBJECT_0 + 1;
    }

    bool mapPixels(ID3D11Resource* resource, uint64_t deadline, D3D11_MAPPED_SUBRESOURCE& mapped,
                   Preparation* timing) noexcept {
        const auto mapStart = timing ? nowUs() : 0;
        for (;;) {
            if (WaitForSingleObject(stopEvent_, 0) != WAIT_TIMEOUT ||
                nowUs() > deadline) return false;
            HRESULT result;
            {
                const auto gateAt = timing ? nowUs() : 0;
                std::lock_guard lock(*gate_);
                if (WaitForSingleObject(stopEvent_, 0) != WAIT_TIMEOUT ||
                    nowUs() > deadline) return false;
                const auto callAt = timing ? nowUs() : 0;
                result = context_->Map(resource, 0, D3D11_MAP_READ,
                                       D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
                if (timing) {
                    timing->mapGateUs += callAt - gateAt;
                    timing->mapCallUs += nowUs() - callAt;
                }
            }
            if (result == DXGI_ERROR_WAS_STILL_DRAWING) {
                if (timing) ++timing->mapRetries;
                if (WaitForSingleObject(stopEvent_, 1) != WAIT_TIMEOUT) return false;
                continue;
            }
            if (FAILED(result)) return false;
            break;
        }

        if (timing) timing->mapUs += nowUs() - mapStart;
        return true;
    }

    void unmapPixels(ID3D11Resource* resource) noexcept {
        std::lock_guard lock(*gate_);
        context_->Unmap(resource, 0);
    }

    bool copyPixels(const Slot& slot, unsigned eye, unsigned char* pixels, uint64_t deadline,
                    Preparation* timing = nullptr) noexcept {
        const auto stereoRowBytes = size_t(slot.width) * 8;
        const auto rowBytes = packedCurrent_ ? stereoRowBytes : stereoRowBytes / 2;
        ID3D11Resource* resource = bufferCurrent_
            ? static_cast<ID3D11Resource*>(linearReadback_.staging(slot.linear)) : slot.staging[eye].Get();
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (!mapPixels(resource, deadline, mapped, timing)) return false;
        const auto copyStart = timing ? nowUs() : 0;
        const bool valid = mapped.pData && (bufferCurrent_ || mapped.RowPitch >= rowBytes);
        if (valid && bufferCurrent_) {
            std::memcpy(pixels, mapped.pData, linearReadback_.byteSize(slot.linear));
        } else if (valid) {
            const auto* source = static_cast<const unsigned char*>(mapped.pData);
            for (unsigned row = 0; row < slot.height; ++row) {
                auto* destination = pixels + size_t(row) * stereoRowBytes;
                const auto* input = source + size_t(row) * mapped.RowPitch;
                if (slot.bgra[eye]) {
                    std::memcpy(destination, input, rowBytes);
                    continue;
                }
                size_t column = 0;
                if (earlyCurrent_) {
                    const auto red = _mm_set1_epi32(0x000000ff);
                    const auto green = _mm_set1_epi32(0x0000ff00);
                    const auto blue = _mm_set1_epi32(0x00ff0000);
                    const auto alpha = _mm_set1_epi32(-16777216);
                    for (; column + 16 <= rowBytes; column += 16) {
                        const auto rgba = _mm_loadu_si128(reinterpret_cast<const __m128i*>(input + column));
                        const auto rb = _mm_or_si128(_mm_slli_epi32(_mm_and_si128(rgba, red), 16),
                                                     _mm_srli_epi32(_mm_and_si128(rgba, blue), 16));
                        const auto bgra = _mm_or_si128(rb, _mm_or_si128(alpha, _mm_and_si128(rgba, green)));
                        _mm_storeu_si128(reinterpret_cast<__m128i*>(destination + column), bgra);
                    }
                }
                for (; column < rowBytes; column += 4) {
                    destination[column] = input[column + 2];
                    destination[column + 1] = input[column + 1];
                    destination[column + 2] = input[column];
                    destination[column + 3] = 255;
                }
            }
        }
        if (timing) timing->copyUs += nowUs() - copyStart;
        unmapPixels(resource);
        return valid;
    }

    bool publish(const Slot& slot) noexcept {
        auto* const shm = slot.shm;
        const auto active = load32(shm);
        const bool ready = load32(shm + 4) != 0;
        if (active > 1 || load32(shm + 32) ||
            (ready && (load32(shm + 8) != slot.width * 2 ||
                       load32(shm + 12) != slot.height ||
                       load64(shm + 40 + 8 * active) > slot.tag))) return false;

        const auto next = 1u - active;
        const auto rowBytes = size_t(slot.width) * 4;
        auto* const pixels = shm + 4096 + size_t(next) * rowBytes * 2 * slot.height;
        const auto deadline = slot.endUs + maximumAgeUs;
        if (!copyPixels(slot, 0, pixels, deadline) || !copyPixels(slot, 1, pixels + rowBytes, deadline) ||
            !waitUntil(slot.endUs + minimumAgeUs)) return false;

        // Fallback is serialized with drain. Check the current slot again
        // before exposing pixels to catch a changed producer or GPU mode.
        std::lock_guard lock(mutex_);
        if (stopping_ || failed_.load() || load32(shm) != active || load32(shm + 32) ||
            nowUs() > slot.endUs + maximumAgeUs) return false;
        store32(shm + 8, slot.width * 2);
        store32(shm + 12, slot.height);
        store64(shm + 40 + 8 * next, slot.tag);
        store32(shm, next);
        store32(shm + 4, 1);
        published_.fetch_add(1);
        return true;
    }

    void retire(Slot& slot, bool complete) noexcept {
        if (complete)
            for (auto& source : slot.input) source.Reset();
        // A failed fence leaves its inputs retained until teardown.
        slot.token = 0;
        ++retired_;
    }

    // Early work writes only owned CPU memory. A missed deadline can return
    // to the stock SHM writer even if this thread is still copying a pair.
    void runEarly() noexcept {
        for (;;) {
            Slot* slot{};
            {
                std::unique_lock lock(mutex_);
                changed_.wait(lock, [&] { return stopping_ || (slot = find(retired_ + 1)); });
                if (stopping_) return;
            }
            Preparation timing{};
            bool complete = mapFirst_ || fenceReady(slot->token, &timing.fenceTimeouts);
            if (!mapFirst_) timing.fenceAt = nowUs();
            bool wanted{};
            {
                std::lock_guard lock(mutex_);
                wanted = !stopping_ && !slot->decided;
            }
            const auto deadline = slot->stagedUs + maximumAgeUs;
            const auto eyeRow = size_t(slot->width) * 4;
            D3D11_MAPPED_SUBRESOURCE mapped{};
            bool hasMapping{}, prepared{};
            if (complete && wanted) {
                if (directMapped_) {
                    hasMapping = mapPixels(linearReadback_.staging(slot->linear), deadline, mapped, &timing);
                    prepared = hasMapping && mapped.pData;
                } else {
                    prepared = copyPixels(*slot, 0, slot->pixels.get(), deadline, &timing) &&
                        (packedCurrent_ || bufferCurrent_ ||
                         copyPixels(*slot, 1, slot->pixels.get() + eyeRow, deadline, &timing));
                }
            }
            // Map's success validates its staging texture. The fence still
            // owns the source textures' lifetime through the final copy.
            if (mapFirst_) {
                complete = fenceReady(slot->token, &timing.fenceTimeouts);
                timing.fenceAt = nowUs();
            }
            if (complete && prepared) timing.finishedAt = nowUs();
            {
                std::unique_lock lock(mutex_);
                slot->mappedPixels = mapped.pData;
                slot->prepared = timing.finishedAt && !slot->decided;
                slot->preparation = timing;
                if (!complete && !stopping_) fail();
                changed_.wait(lock, [&] { return stopping_ || slot->decided; });
                if (hasMapping) {
                    slot->mappedPixels = nullptr;
                    lock.unlock();
                    unmapPixels(linearReadback_.staging(slot->linear));
                    lock.lock();
                }
                if (stopping_) return;
                retire(*slot, complete);
            }
            changed_.notify_all();
        }
    }

    void run() noexcept {
        if (earlyCurrent_) { runEarly(); return; }
        for (;;) {
            Slot* slot{};
            {
                std::unique_lock lock(mutex_);
                changed_.wait(lock, [&] {
                    slot = find(retired_ + 1);
                    return stopping_ || (slot && slot->decided);
                });
                if (stopping_) return;
            }

            const bool complete = fenceReady(slot->token);
            if (!complete || (slot->accepted && !failed_.load() && !publish(*slot))) {
                if (WaitForSingleObject(stopEvent_, 0) == WAIT_TIMEOUT) {
                    if (complete && slot->accepted && nowUs() > slot->endUs + maximumAgeUs)
                        expired_.fetch_add(1);
                    else fail();
                }
            }

            {
                std::lock_guard lock(mutex_);
                retire(*slot, complete);
            }
            changed_.notify_all();
        }
    }

    void stop() noexcept {
        {
            std::lock_guard lock(mutex_);
            stopping_ = true;
        }
        if (stopEvent_) SetEvent(stopEvent_);
        changed_.notify_all();
        if (worker_.joinable()) worker_.join();

        const auto complete = fence_ ? fence_->GetCompletedValue() : 0;
        const bool unresolved = untrackedCopy_ || (complete != UINT64_MAX && complete < issued_);
        if (unresolved) {
            // Do not free a D3D object with an unproven submitted GPU read.
            // This is only reached on device/driver failure or forced stop.
            for (auto& slot : slots_) {
                for (unsigned eye = 0; eye < slot.input.size(); ++eye)
                    if (slot.input[eye]) {
                        (void)slot.input[eye].Detach();
                        (void)slot.staging[eye].Detach();
                    }
                if (bufferCurrent_ && slot.linear.output) {
                    (void)slot.linear.output.Detach();
                    (void)slot.linear.readback.Detach();
                    (void)slot.linear.config.Detach();
                    (void)slot.linear.outputView.Detach();
                    for (auto& view : slot.linear.inputViews) (void)view.Detach();
                    for (auto& texture : slot.linear.samplingTextures) (void)texture.Detach();
                }
            }
            // A pending fence registration may still signal this event.
            // Leave it and its fence alive rather than racing handle reuse.
            (void)fence_.Detach();
        }
        if (timer_) CloseHandle(timer_);
        if (stopEvent_) CloseHandle(stopEvent_);
        if (fenceEvent_ && !unresolved) CloseHandle(fenceEvent_);
        timer_ = stopEvent_ = fenceEvent_ = nullptr;
    }

public:
    MoltenEventWorker(ID3D11Device* device, ID3D11DeviceContext* context,
                     xrfg::ContextEncodingGate* gate, bool earlyCurrent = false,
                     bool packedCurrent = false, bool mapFirst = false, bool bufferCurrent = false,
                     bool directMapped = false, uint64_t fenceWaitBudgetUs = 70'000)
        : device_(device), gate_(gate), earlyCurrent_(earlyCurrent), packedCurrent_(earlyCurrent && packedCurrent),
          mapFirst_(earlyCurrent && mapFirst), bufferCurrent_(earlyCurrent && bufferCurrent),
          directMapped_(earlyCurrent && bufferCurrent && directMapped), fenceWaitBudgetUs_(fenceWaitBudgetUs) {
        if (context) context->QueryInterface(IID_PPV_ARGS(&context_));
    }

    MoltenEventWorker(const MoltenEventWorker&) = delete;
    MoltenEventWorker& operator=(const MoltenEventWorker&) = delete;
    ~MoltenEventWorker() { stop(); }

    [[nodiscard]] bool initialize() {
        if (initialized_ || stopping_ || fence_ || !device_ || !context_ || !gate_) return false;
        Microsoft::WRL::ComPtr<ID3D11Device5> device5;
        if (FAILED(device_.As(&device5)) ||
            FAILED(device5->CreateFence(0, D3D11_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_)))) return false;
        if (bufferCurrent_ && !linearReadback_.initialize(device_.Get())) return false;
        fenceEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        timer_ = CreateWaitableTimerExW(nullptr, nullptr, 2, TIMER_ALL_ACCESS);
        if (!timer_) timer_ = CreateWaitableTimerW(nullptr, FALSE, nullptr);
        if (!fenceEvent_ || !stopEvent_ || !timer_) return false;
        try {
            worker_ = std::thread(&MoltenEventWorker::run, this);
        } catch (...) {
            return false;
        }
        initialized_ = true;
        return true;
    }

    [[nodiscard]] uint64_t stage(const std::array<ID3D11Texture2D*, 2>& textures,
                                 unsigned width, unsigned height, unsigned char* shm,
                                 uint64_t tag) {
        if (!initialized_ || failed_.load() || !shm || !tag ||
            !width || width > 16'384 || !height || height > 16'384) return 0;
        if (load32(shm) > 1 || load32(shm + 32) ||
            (load32(shm + 4) &&
             (load32(shm + 8) != width * 2 || load32(shm + 12) != height))) return 0;

        std::array<D3D11_TEXTURE2D_DESC, 2> desc{};
        std::array<bool, 2> bgra{};
        for (unsigned eye = 0; eye < textures.size(); ++eye)
            if (!describe(textures[eye], width, height, desc[eye], bgra[eye])) return 0;
        if (packedCurrent_) {
            if (desc[0].Format != desc[1].Format || width > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION / 2)
                return 0;
            desc[0].Width *= 2;
        }

        std::lock_guard lock(mutex_);
        if (stopping_ || failed_.load() || issued_ >= UINT64_MAX - 1) return 0;
        // Real and synthesized outputs can alternate RGBA/BGRA on every
        // tick. Keep one warmed staging pair per format when possible.
        auto* slot = select(desc);
        if (!slot) return 0;
        if (!bufferCurrent_ && (!prepare(slot->staging[0], desc[0]) ||
            (!packedCurrent_ && !prepare(slot->staging[1], desc[1]))))
            return 0;
        const auto bytes = size_t(width) * height * 8;
        if (earlyCurrent_ && !directMapped_ && slot->pixelCapacity < bytes) {
            auto pixels = std::unique_ptr<unsigned char[]>(new (std::nothrow) unsigned char[bytes]);
            if (!pixels) return 0;
            slot->pixels = std::move(pixels);
            slot->pixelCapacity = bytes;
        }

        slot->width = width;
        slot->height = height;
        slot->bgra = bgra;
        slot->shm = shm;
        slot->tag = tag;
        slot->mappedPixels = nullptr;
        slot->decided = slot->accepted = slot->prepared = false;
        slot->preparation = {};
        slot->stagedUs = nowUs();
        if (bufferCurrent_ && !linearReadback_.queue(context_.Get(), slot->linear, textures, width, height)) return 0;
        const D3D11_BOX eyeBox{0, 0, 0, width, height, 1};
        for (unsigned eye = 0; eye < textures.size(); ++eye) {
            slot->input[eye] = textures[eye];
            if (bufferCurrent_) continue;
            if (packedCurrent_)
                context_->CopySubresourceRegion(slot->staging[0].Get(), 0, eye * width, 0, 0,
                                                textures[eye], 0, &eyeBox);
            else context_->CopyResource(slot->staging[eye].Get(), textures[eye]);
        }
        const auto token = issued_ + 1;
        if (FAILED(context_->Signal(fence_.Get(), token))) {
            untrackedCopy_ = true;
            fail();
            return 0;
        }
        context_->Flush();
        issued_ = slot->token = token;
        if (earlyCurrent_) changed_.notify_all();
        return token;
    }

    // The presenter decides before native End. The worker only reads/writes
    // private resources, so neither test nor fallback waits on a GPU fence.
    [[nodiscard]] Preparation earlyPreparation(uint64_t token, const unsigned char* shm,
                                               unsigned width, unsigned height) noexcept {
        std::unique_lock lock(mutex_, std::try_to_lock);
        if (!token || !earlyCurrent_ || !lock.owns_lock() || stopping_ || failed_.load()) return {};
        const auto* slot = find(token);
        return slot && slot->prepared && !slot->decided && slot->shm==shm &&
            slot->width==width && slot->height==height ? slot->preparation : Preparation{};
    }

    bool publishEarly(uint64_t token, unsigned char* shm, unsigned width, unsigned height) noexcept {
        std::lock_guard lock(mutex_);
        auto* slot = token ? find(token) : nullptr;
        if (!earlyCurrent_ || !slot || !slot->prepared || slot->decided ||
            slot->shm!=shm || slot->width!=width || slot->height!=height) return false;
        const auto active = load32(shm);
        const bool ready = load32(shm + 4) != 0;
        if (active > 1 || load32(shm + 32) ||
            (ready && (load32(shm + 8) != slot->width * 2 || load32(shm + 12) != slot->height ||
                       load64(shm + 40 + 8 * active) > slot->tag))) return false;
        const auto next = 1u - active;
        const auto bytes = size_t(slot->width) * slot->height * 8;
        const void* pixels = directMapped_ ? slot->mappedPixels : slot->pixels.get();
        if (!pixels) return false;
        std::memcpy(shm + 4096 + next * bytes, pixels, bytes);
        store32(shm + 8, slot->width * 2);
        store32(shm + 12, slot->height);
        store64(shm + 40 + 8 * next, slot->tag);
        store32(shm, next);
        store32(shm + 4, 1);
        published_.fetch_add(1);
        slot->decided = true;
        changed_.notify_all();
        return true;
    }

    void finish(uint64_t token, bool accept, uint64_t nativeEndUs) noexcept {
        if (!token) return;
        {
            std::lock_guard lock(mutex_);
            auto* slot = find(token);
            if (!slot || slot->decided) return;
            slot->decided = true;
            slot->accepted = accept && nativeEndUs;
            slot->endUs = nativeEndUs;
        }
        changed_.notify_all();
    }

    // True means no worker can still write pixels or metadata from an older
    // token. On a timeout the caller must not hand the mapping to another writer.
    [[nodiscard]] bool drain() noexcept {
        std::unique_lock lock(mutex_);
        const bool quiescent = changed_.wait_for(lock, std::chrono::milliseconds(250),
                                                [&] { return retired_ == issued_; });
        if (!quiescent) fail();
        return quiescent;
    }

    void shutdown() noexcept { stop(); }
    [[nodiscard]] uint64_t expired_count() const noexcept { return expired_.load(); }
    [[nodiscard]] uint64_t published_count() const noexcept { return published_.load(); }
    [[nodiscard]] uint64_t failure_count() const noexcept { return failures_.load(); }
};

} // namespace runtime_study
