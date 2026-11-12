#pragma once

#include <array>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <vector>
#include <optional>
#include <functional>
#include <cstdint>
#include <windows.h>
#include <d3d11_4.h>
#include <wrl/client.h>
#include <d3d12.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include "xrfg/private_output_distributor.hpp"
#include "xrfg/context_encoding_gate.hpp"

namespace xrfg {
struct InterfaceDeliveryReceipt;
// Start optional work after real copy submission, then budget its admission
// against the next refresh after real End. Neither edge proves scanout.
class FrameCopyReceipt final {
    std::mutex mutex_;
    std::condition_variable changed_;
    bool submitted_{},native_finished_{};
    std::int64_t native_entry_ns_{};
public:
    // Set and read on the submitting thread.
    std::int64_t reservation_wait_ns{};
    bool wait_until(std::chrono::steady_clock::time_point deadline) {
        std::unique_lock lock(mutex_);
        return changed_.wait_until(lock,deadline,[&]{return submitted_;});
    }
    void mark() {
        {std::scoped_lock lock(mutex_);submitted_=true;}
        changed_.notify_all();
    }
    void mark_native_end(std::int64_t entryNs) {
        {std::scoped_lock lock(mutex_);native_entry_ns_=entryNs;native_finished_=true;}
        changed_.notify_all();
    }
    std::int64_t wait_native_end_until(std::chrono::steady_clock::time_point deadline) {
        std::unique_lock lock(mutex_);
        if(!changed_.wait_until(lock,deadline,[&]{return native_finished_;}))return 0;
        return native_entry_ns_;
    }
};
// Downstream functions are the REAL runtime, never the layer intercepts.
struct PrivateRuntimeDispatch {
    XrInstance instance{};
    XrSession session{};
    PFN_xrWaitFrame wait{};
    PFN_xrBeginFrame begin{};
    PFN_xrEndFrame end{};
    PFN_xrCreateSwapchain create{};
    PFN_xrDestroySwapchain destroy{};
    PFN_xrEnumerateSwapchainImages enumerate{};
    PFN_xrAcquireSwapchainImage acquire{};
    PFN_xrWaitSwapchainImage wait_image{};
    PFN_xrReleaseSwapchainImage release{};
    PFN_xrConvertTimeToWin32PerformanceCounterKHR to_qpc{};
};

// SteamVR Begin/End and image release belong to the consumer; one independent
// Wait worker prepares the next runtime clock. All application/generator
// images are private, with leases (not copies) transferred at virtual EndFrame.
// Source read leases end only after the copy GPU fence completes. Real target
// ownership and FIFO credits remain until native End; source generations prevent
// a rewritten private image from reusing an older prepared copy.
// A preparer owns real acquisition/copy/GPU waits; the physical frame thread
// consumes completed packets only and releases that FIFO head's real targets.
// The next image may be acquired/waited/copied while an older RELEASED image
// is presented, but stays unreleased until its own EndFrame can capture it.
// This retains last-released image semantics without additional XR swapchains.
// Native D3D11 and D3D12 are supported; existing Vulkan/D3D11 translation binds
// D3D12 below this endpoint and therefore uses the identical D3D12 transport.
class PrivateOpenXrRuntime final {
    struct Impl;
public:
    struct Options {
        struct ApplicationReleaseTiming {
            std::int64_t end_ns{}, stereo_released_ns{}, fence_observed_ns{}, fence_complete{};
        };
        struct RealPreparationTiming {
            std::int64_t app_gpu_observed_ns{}, linear_stage_enter_ns{}, linear_stage_return_ns{};
            std::int64_t linear_fence_observed_ns{}, linear_ready_ns{}, linear_map_call_ns{}, linear_map_gate_ns{};
            std::int64_t native_targets_ready_ns{}, native_copy_enter_ns{};
            std::int64_t native_copy_submitted_ns{}, native_copy_complete_ns{};
        };
        struct ApplicationStageTiming {
            std::int64_t frame_start_ns{}, pose_sample_ns{};
        };
        struct PublicationTiming {
            std::uint64_t sequence;
            std::uint64_t synthetic;
            std::int64_t source_time, accepted_ns, prepared_ns, native_ready_ns;
            std::int64_t release_ready_ns, wait_return_ns, predicted_time;
            std::int64_t predicted_qpc_ns, period_ns, actual_handoff_ns, native_end_return_ns;
            ApplicationReleaseTiming application;
            RealPreparationTiming preparation;
            ApplicationStageTiming app_stages;
        };
        struct D3D12LayerImage {
            ID3D12Resource* resource{};
            DXGI_FORMAT format{DXGI_FORMAT_UNKNOWN};
            D3D12_RESOURCE_STATES state{};
        };
        PrivateRuntimeDispatch real;
        ID3D12Device* device12{};
        ID3D12CommandQueue* queue12{}; // the actual runtime binding queue
        ID3D12CommandQueue* producer_queue12{}; // private writes, never future work on queue12
        // Called on the copy list before native Release. Return false without
        // recording any commands to keep the original OpenXR layers intact.
        std::function<bool(ID3D12GraphicsCommandList*, XrTime, const XrCompositionLayerProjection&,
            const std::array<D3D12LayerImage, 2>&, const XrCompositionLayerQuad&, const D3D12LayerImage&)>
            compose_d3d12_quad;
        ID3D11Device* device11{};
        ID3D11DeviceContext* context11{};
        std::size_t capacity{2};
        std::function<std::int64_t()> physical_period;
        // Cached physical vsync only. Must not call a runtime, lock an observer
        // or wait from the consumer. Empty until the session clock is known.
        std::function<std::optional<PrivatePhysicalClock>()> physical_clock;
        std::function<bool()> trace_enabled;
        // stage: 1 accepted, 2 accepted by real runtime, -1 failed; sequence,
        // QPC ns and accumulated phase displacement (not a latency meter).
        // Stage3 instead carries submitted and raw predicted XR displayTime.
        // Stages4/5: preparer source GPU ready / real image copy complete,
        // with the real target still waited/unreleased until FIFO delivery.
        // Stages7/6: saved real Wait entry/return (overlaps preparation, emitted
        // when the ready packet is known). Stage8: saved handoff edge
        // after the residual wait/copy, emitted after real End to keep the edge
        // free of diagnostic work. QPC ns, c=0. Stage3 runs in the early window.
        // Stage10: input readiness before the real-chain preparation gate;
        // c=1 fence completion observed, c=0 acceptance lower bound when
        // already complete at dequeue. Stage11: underrun refill, QPC ns and
        // exact margin retained after the late frame for subsequent deliveries.
        // A real-chain backlog never triggers a refill.
        // Stages12/13/14: targets acquired/waited, copy submitted/signalled,
        // and copy-fence completion observed. F9-only CPU QPC observations,
        // not GPU execution timestamps. Real release follows on the consumer,
        // after stage5 and before this packet's native EndFrame.
        // Stages15/16: release entry QPC/duration, Begin-complete QPC/next
        // native-entry budget. Optional CPU observations, never GPU queries.
        // Stage17: exact native-margin deficit retained in the output phase.
        // Stage18: native Wait entry QPC and worker thread ID; a is the
        // per-start accepted-input ordinal (0 for the seed), not packet ID.
        // V593: ordinal0 also identifies unsubmitted native-clock renewals.
        // Stage23: packet ID, expired raw prediction converted to QPC, planned
        // handoff QPC. Stage24: final raw prediction QPC and renewed-cycle count.
        // Renewals keep the same owned packet; only the old native Begin is
        // discarded through OpenXR's XR_FRAME_DISCARDED transition, without End.
        // Stage25: one preventive native-window renewal, raw prediction QPC
        // and required display-edge+guard coverage (V651). This is bounded to one
        // attempt per packet, separate from expired-cycle recovery. A still-current
        // replacement may retain a shorter horizon; never spin for that preference.
        // Stage27: allocation-only publication mapping; source handle in a,
        // virtual output handle in b and native sink in c (not QPC times).
        // Stage26: source read leases released after completed copy; packet ID,
        // QPC ns and lease count. This does not retire real targets or FIFO credits.
        std::function<void(int,std::uint64_t,std::int64_t,std::int64_t)> trace;
        std::function<void(std::int64_t)> accepted_output; // native End success: ORIGINAL source XrTime displayTime, never host QPC or enqueue time
        std::function<void()> accepted_synthetic_output; // native End accepted a generated image
        // Optional observation after a successful content handoff. No disk
        // operations or additional GPU waits on the presenter thread.
        std::function<void(const PublicationTiming&)> publication_timing;
        // Optional stereo source-release measurements in the same QPC domain.
        bool trace_application_release{};
        bool trace_real_preparation{};
        std::function<ApplicationStageTiming(std::int64_t)> application_stage_stamps;
        bool director_enabled{true}; // false: ordered native-runtime paced bypass
        // Luke D11/SteamVR only. Freeze image readiness at Release, not after
        // newer game commands at End. Admission/work budget is selected separately.
        bool d11_release_readiness{};
        // MoltenVR-specific: encoded head-pose tags are keyed by the source
        // LocateViews displayTime. Native clock ownership stays with the worker,
        // while content retains the exact source timestamp; off for other runtimes.
        bool preserve_source_display_time{};
        // Optional runtime-specific Wait-only lookahead. The sole already-posted
        // native Wait may enter before accepted/prepared content exists. Begin,
        // End and image ownership remain exclusively on the normal consumer.
        // One cached Wait is balanced at stop, including after a loading pause.
        bool native_wait_lookahead{};
        // Ordinary fixed multiplier metadata. Actual source production remains
        // coupled to completed output-pair/FIFO ownership; no independent timer.
        std::uint32_t source_frame_multiplier{1};
        bool reset_source_multiplier_on_start{};
        // Runtime-specific native delivery spacing, independent of the optional
        // director. A late completed packet never causes a catch-up burst.
        bool uniform_output{};
        // Forward generation can space the forecast without holding a fresh
        // game-rendered frame behind a late forecast.
        bool pace_synthetic_only{};
        bool shared_forward_cadence{};
        bool molten_event_publication{};
        bool early_linear_publication{};
        std::int64_t end_phase_advance_ns{};
        std::int64_t end_phase_delay_ns{};
        bool stage_event_before_native_copy{};
        // Internal capture extension. Stage only while both native target
        // images are waited and unreleased. End owns a nonzero token.
        std::function<std::uint64_t(const XrFrameEndInfo&,
            const std::array<ID3D11Texture2D*, 2>&)> stage_event;
        // The final output flag records content published separately from an
        // empty native EndFrame on the event path.
        std::function<XrResult(std::uint64_t, const XrFrameEndInfo&, RealPreparationTiming*, bool&)> end_event;
        std::function<void(std::uint64_t)> cancel_event;
        bool native_application_color{};
        // Cached post-opaque-gate native End entry in QPC nanoseconds. Read only:
        // this callback must not enter a lock, runtime or graphics API.
        std::function<std::int64_t()> native_handoff_at;
        // Optional external gate for opaque native encoding on the producer.
        // Copy submission uses the gate; GPU completion waits never hold it.
        ContextEncodingGate* opaque_context_mutex{};
    };
    explicit PrivateOpenXrRuntime(Options);
    ~PrivateOpenXrRuntime();
    PrivateOpenXrRuntime(const PrivateOpenXrRuntime&)=delete;
    PrivateOpenXrRuntime& operator=(const PrivateOpenXrRuntime&)=delete;
    HRESULT initialize();
    XrResult start(); // after successful real xrBeginSession
    void stop(); // wakes producers; consumer closes/joins native Wait; drains GPU reads
    XrResult drain();
    // Stop-only cancellation of future deferred-source output admission. Already
    // accepted packets drain normally; ordinary profile changes do not cancel.
    void pause_deferred_admissions(bool paused) noexcept;
    // Drain accepted packets and change only final delivery policy. Session,
    // native binding, private images and worker ownership remain unchanged.
    XrResult set_director_enabled(bool);
    XrResult set_source_multiplier(std::uint32_t);
    [[nodiscard]] std::uint32_t source_multiplier() const noexcept;
    XrResult set_uniform_output(bool);
    [[nodiscard]] bool uniform_output_enabled() const noexcept;
    [[nodiscard]] std::int64_t next_source_release_ns() const noexcept;
    XrResult set_buffer_capacity(std::size_t);
    [[nodiscard]] std::size_t buffer_capacity() const noexcept;
    [[nodiscard]] bool director_enabled() const noexcept;
    XrResult wait_frame(const XrFrameWaitInfo*, XrFrameState*);
    XrResult predict_frame(XrFrameState*); // upper virtual application clock
    XrResult begin_frame(const XrFrameBeginInfo*);
    // A deferred source owns its metadata AND exact released-image indices.
    // A swapchain handle alone is insufficient: a newer frame may release another image.
    class LeasedFrame final {
        friend class PrivateOpenXrRuntime;
        std::shared_ptr<void> storage_;
        const XrFrameEndInfo* info_{};
        const void* owner_{};std::uint64_t epoch_{};
        LeasedFrame()=default;
    public:
        const XrFrameEndInfo* info()const noexcept{return info_;}
    };
    XrResult defer_frame(const XrFrameEndInfo*,std::shared_ptr<LeasedFrame>&,
                         std::int64_t application_end_ns=0);
    // Exact source resource pinned by this frame, never the chain's latest release.
    // Empty on a foreign/stale token, absent chain or non-D3D11 source. The COM
    // reference retains the object only: keep LeasedFrame alive through GPU read
    // completion to prevent the application from acquiring/rewriting its pixels.
    // Optional declared_format receives this frozen chain's XR format (including
    // for typeless D3D resources), or zero on every rejected/unsupported source.
    [[nodiscard]] Microsoft::WRL::ComPtr<ID3D11Texture2D> source_image11(
        const LeasedFrame&,XrSwapchain,std::int64_t* declared_format=nullptr) const;
    struct D3D12SourceImage {
        Microsoft::WRL::ComPtr<ID3D12Resource> texture;
        Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue;
        std::int64_t declared_format{};
        D3D12_RESOURCE_STATES release_state{D3D12_RESOURCE_STATE_COMMON};

    private:
        friend class PrivateOpenXrRuntime;
        std::shared_ptr<const LeasedFrame> lease_;
    };
    // Submit reads only on the returned OpenXR binding queue. Its ordering
    // puts them after application writes and orders them with native copies.
    // Restore release_state in the same command list. Keep the whole receipt
    // through the read fence, and drain submitted reads before stop/restart.
    // A separate producer queue requires explicit cross-queue synchronization
    // and cannot obtain this same-queue receipt.
    [[nodiscard]] std::optional<D3D12SourceImage> source_image12(
        const std::shared_ptr<const LeasedFrame>&, XrSwapchain) const;
    class D3D12OutputImages final {
    public:
        struct Eye {
            XrSwapchain swapchain{};
            Microsoft::WRL::ComPtr<ID3D12Resource> texture;
            std::int64_t declared_format{};
            XrSwapchainSubImage subImage{};
        };
        D3D12OutputImages(D3D12OutputImages&&) noexcept = default;
        D3D12OutputImages& operator=(D3D12OutputImages&&) = delete;
        ~D3D12OutputImages();
        [[nodiscard]] const std::array<Eye, 2>& eyes() const noexcept { return eyes_; }
        [[nodiscard]] ID3D12CommandQueue* queue() const noexcept { return queue_.Get(); }
        // Call only after submission on queue() has restored both textures to
        // RENDER_TARGET. The existing frame enqueue orders native copies after
        // that submission. Abandoning does not change the last released image.
        [[nodiscard]] bool publish() noexcept;

    private:
        friend class PrivateOpenXrRuntime;
        D3D12OutputImages() = default;
        bool finish(bool publish) noexcept;
        std::shared_ptr<Impl> owner_;
        std::array<Eye, 2> eyes_{};
        Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue_;
        std::array<std::uint32_t, 2> indices_{};
        std::uint64_t epoch_{}, ticket_{};
    };
    XrResult create_output_swapchain12(std::uint32_t width, std::uint32_t height, DXGI_FORMAT, XrSwapchain*);
    XrResult create_matching_output_swapchain12(XrSwapchain source, XrSwapchain*);
    [[nodiscard]] bool output_matches_source12(XrSwapchain output, XrSwapchain source) const;
    XrResult try_destroy_output_swapchain12(XrSwapchain);
    // Reserve both private eye images together, without waiting on a busy ring
    // or acquiring any native OpenXR image on the generator thread. Two
    // subimages of the same texture share one write lease.
    [[nodiscard]] std::optional<D3D12OutputImages> try_acquire_output_images12(
        const std::array<XrSwapchainSubImage, 2>&);
    XrResult protect_application_reads(const LeasedFrame&,bool& protectedNative);
    // Internal outputs of one previously accepted source. Does not consume the
    // application's next Wait/Begin. Original/UI handles use frozen image leases.
    // Optional absolute QPC ns cutoff bounds synthetic admission. A missed
    // cutoff returns XR_TIMEOUT_EXPIRED and leaves the native cycle intact.
    XrResult end_deferred_output(const XrFrameEndInfo*,const LeasedFrame&,
        const std::shared_ptr<FrameCopyReceipt>& copyReceipt={},std::int64_t admitBeforeNs=0);
    XrResult end_frame(const XrFrameEndInfo*,
        const std::shared_ptr<InterfaceDeliveryReceipt>& delivery={});
    XrResult create_swapchain(const XrSwapchainCreateInfo*, XrSwapchain*,
        D3D12_RESOURCE_STATES image_state=D3D12_RESOURCE_STATE_RENDER_TARGET,
        std::uint32_t image_count=3,
        // Internal generated outputs only: compatible rings of one source eye
        // share a native sink; the producer handle/images remain independent.
        XrSwapchain publication_source=XR_NULL_HANDLE,
        // Internal D3D11 synthetic output only. The converter writes the
        // exact native image, which stays waited until FIFO delivery.
        bool direct_synthetic=false);
    XrResult destroy_swapchain(XrSwapchain);
    XrResult enumerate_images(XrSwapchain,std::uint32_t,std::uint32_t*,XrSwapchainImageBaseHeader*);
    XrResult acquire_image(XrSwapchain,const XrSwapchainImageAcquireInfo*,std::uint32_t*);
    XrResult wait_image(XrSwapchain,const XrSwapchainImageWaitInfo*);
    XrResult release_image(XrSwapchain,const XrSwapchainImageReleaseInfo*);
    // Completion proof for the EXACT acquired output; never a guessed serial.
    // Caller already CPU-observed this publication's fence. Unproven/foreign
    // images still go through release_image and its original-context marker.
    XrResult release_completed_image(XrSwapchain,const XrSwapchainImageReleaseInfo*,
        std::uint32_t index,ID3D11Texture2D* published_image,ID3D12Fence*,std::uint64_t value);
    [[nodiscard]] bool owns(XrSwapchain) const;
    struct SwapchainMapping { XrSwapchain handle{}, native{}; };
    // CPU-only inventory; busy transport returns no partial snapshot.
    [[nodiscard]] std::optional<std::vector<SwapchainMapping>> native_swapchain_mapping() const;
private:
    std::shared_ptr<Impl> impl_;
    XrResult create_swapchain_impl(const XrSwapchainCreateInfo*, XrSwapchain*,
        D3D12_RESOURCE_STATES, std::uint32_t, XrSwapchain, bool direct_synthetic, bool output12,
        const D3D12_RESOURCE_DESC* match_output12 = nullptr);
    XrResult destroy_swapchain_impl(XrSwapchain, bool try_output12);
    XrResult enqueue_frame(const XrFrameEndInfo*,const std::shared_ptr<InterfaceDeliveryReceipt>&,
        bool,const LeasedFrame*,const std::shared_ptr<FrameCopyReceipt>& copyReceipt={},
        std::int64_t admitBeforeNs=0);
};
} // namespace xrfg
