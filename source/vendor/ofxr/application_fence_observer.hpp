#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <thread>
#include <windows.h>
#include <d3d11_4.h>
#include <wrl/client.h>

namespace xrfg {

// Diagnostic observer for an existing D3D11 fence. The application submits only
// a serial. No graphics context calls or waits run on the application's path.
class ApplicationFenceObserver {
    using Clock = std::int64_t (*)() noexcept;
    static constexpr std::size_t pending_capacity = 8;
    static constexpr std::size_t observed_capacity = 128;

    struct Observation {
        std::atomic<std::uint64_t> serial{0};
        std::atomic<std::int64_t> observed_ns{0};
    };

    Microsoft::WRL::ComPtr<ID3D11Fence> fence_;
    Clock clock_;
    HANDLE stop_event_{}, fence_event_{};
    std::thread worker_;
    std::mutex mutex_;
    std::condition_variable changed_;
    std::array<std::uint64_t, pending_capacity> pending_{};
    std::size_t head_{}, count_{};
    bool stopping_{};
    std::uint64_t outstanding_{};  // Worker-owned until join.
    std::array<Observation, observed_capacity> observations_{};

    void run() noexcept {
        const HANDLE events[]{stop_event_, fence_event_};
        for (;;) {
            std::uint64_t serial{};
            {
                std::unique_lock lock(mutex_);
                changed_.wait(lock, [&] { return stopping_ || count_; });
                if (stopping_) return;
                serial = pending_[head_];
                head_ = (head_ + 1) % pending_capacity;
                --count_;
            }
            const auto completed = fence_->GetCompletedValue();
            if (completed == UINT64_MAX) return;
            // A completion that predates registration has no event timestamp.
            if (completed >= serial) continue;
            if (FAILED(fence_->SetEventOnCompletion(serial, fence_event_))) return;
            outstanding_ = serial;
            for (;;) {
                if (WaitForMultipleObjects(2, events, FALSE, INFINITE) != WAIT_OBJECT_0 + 1) return;
                const auto ready = fence_->GetCompletedValue();
                if (ready == UINT64_MAX) return;
                if (ready >= serial) break;
            }
            const auto at = clock_();
            outstanding_ = 0;
            auto& slot = observations_[serial % observed_capacity];
            slot.serial.store(0);
            slot.observed_ns.store(at);
            slot.serial.store(serial);
        }
    }

public:
    ApplicationFenceObserver(ID3D11Fence* fence, Clock clock) noexcept : fence_(fence), clock_(clock) {
        stop_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        fence_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (stop_event_ && fence_event_) {
            try { worker_ = std::thread([this] { run(); }); }
            catch (...) { /* Diagnostics can be unavailable without failing the session. */ }
        }
    }
    ApplicationFenceObserver(const ApplicationFenceObserver&) = delete;
    ApplicationFenceObserver& operator=(const ApplicationFenceObserver&) = delete;

    ~ApplicationFenceObserver() {
        {
            std::lock_guard lock(mutex_);
            stopping_ = true;
        }
        if (stop_event_) SetEvent(stop_event_);
        changed_.notify_one();
        if (worker_.joinable()) worker_.join();
        if (stop_event_) CloseHandle(stop_event_);
        if (fence_event_) {
            const auto completed = outstanding_ ? fence_->GetCompletedValue() : 0;
            // Runtime shutdown normally already drained this fence. If device
            // failure left an unresolved registration, retain its handle.
            if (!outstanding_ || (completed != UINT64_MAX && completed >= outstanding_))
                CloseHandle(fence_event_);
        }
    }

    void submit(std::uint64_t serial) noexcept {
        if (!serial || !worker_.joinable()) return;
        {
            std::unique_lock lock(mutex_, std::try_to_lock);
            if (!lock || stopping_ || count_ == pending_capacity) return;
            pending_[(head_ + count_) % pending_capacity] = serial;
            ++count_;
        }
        changed_.notify_one();
    }

    [[nodiscard]] std::int64_t observed(std::uint64_t serial) const noexcept {
        if (!serial) return 0;
        const auto& slot = observations_[serial % observed_capacity];
        if (slot.serial.load() != serial) return 0;
        const auto at = slot.observed_ns.load();
        return slot.serial.load() == serial ? at : 0;
    }
};

} // namespace xrfg
