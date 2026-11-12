#pragma once

#include <windows.h>
#include <dxgiformat.h>
#include <openxr/openxr.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace runtime_study::menu_framegen {
inline constexpr char layerName[] = "XR_APILAYER_ARMAX_framegen";

inline std::optional<bool> readMode() {
    HANDLE file = CreateFileW(L"C:\\FrameGen\\mode.txt", GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return {};
    char mode[8]{};
    DWORD count{};
    const bool read = ReadFile(file, mode, sizeof mode, &count, nullptr) != 0;
    CloseHandle(file);
    if (!read) return {};
    auto text = std::string_view(mode, count);
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r'))
        text.remove_suffix(1);
    if (text == "on") return true;
    if (text == "off") return false;
    return {};
}

inline std::string executableName() {
    std::wstring executable(32768, L'\0');
    const auto length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    if (!length || length >= executable.size()) return "OpenXR application";
    executable.resize(length);
    const auto separator = executable.find_last_of(L"\\/");
    if (separator != std::wstring::npos) executable.erase(0, separator + 1);
    for (auto& c : executable) if (c < L' ' || c == 127) c = L' ';
    const auto bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, executable.c_str(), -1,
        nullptr, 0, nullptr, nullptr);
    if (bytes <= 1) return "OpenXR application";
    std::string name(bytes, '\0');
    if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, executable.c_str(), -1,
        name.data(), bytes, nullptr, nullptr)) return "OpenXR application";
    name.pop_back();
    return name;
}

enum class Status { off, waiting, unsupported, unsupported_d3d12, active };

enum class BlockReason : std::uint32_t {
    none, unsupported_graphics, context_protection, context_missing, context_identity,
    device_protection_only, transport_init, generation_init,
    transport_start, worker_start, source_failure, color_fault, color_source, color_output,
    frame_extensions, blend_mode, layer_count, layer_type, layer_extensions, layer_flags,
    view_count, view_extensions, chain_missing, color_format, color_samples, color_layout,
    color_rect, required_output, dx12_queue, dx12_owner, dx12_dispatch, dx12_presenter,
    dx12_root, dx12_shader, dx12_pipeline, dx12_commands, dx12_descriptors, dx12_constants
};

struct AdmissionIssue {
    BlockReason reason = BlockReason::none;
    std::int32_t detail = 0;
    bool operator==(const AdmissionIssue&) const = default;
};

struct StatusSnapshot {
    Status state;
    AdmissionIssue issue{};
    bool operator==(const StatusSnapshot&) const = default;
};

inline const char* blockReasonName(BlockReason reason) {
    switch (reason) {
    case BlockReason::none: return "none";
    case BlockReason::unsupported_graphics: return "unsupported-graphics";
    case BlockReason::context_protection: return "context-protection";
    case BlockReason::context_missing: return "context-missing";
    case BlockReason::context_identity: return "context-identity";
    case BlockReason::device_protection_only: return "device-protection-only";
    case BlockReason::transport_init: return "transport-init";
    case BlockReason::generation_init: return "generation-init";
    case BlockReason::transport_start: return "transport-start";
    case BlockReason::worker_start: return "worker-start";
    case BlockReason::source_failure: return "source-failure";
    case BlockReason::color_fault: return "color-fault";
    case BlockReason::color_source: return "color-source";
    case BlockReason::color_output: return "color-output";
    case BlockReason::frame_extensions: return "frame-extensions";
    case BlockReason::blend_mode: return "blend-mode";
    case BlockReason::layer_count: return "layer-count";
    case BlockReason::layer_type: return "layer-type";
    case BlockReason::layer_extensions: return "layer-extensions";
    case BlockReason::layer_flags: return "layer-flags";
    case BlockReason::view_count: return "view-count";
    case BlockReason::view_extensions: return "view-extensions";
    case BlockReason::chain_missing: return "chain-missing";
    case BlockReason::color_format: return "color-format";
    case BlockReason::color_samples: return "color-samples";
    case BlockReason::color_layout: return "color-layout";
    case BlockReason::color_rect: return "color-rect";
    case BlockReason::required_output: return "required-output";
    case BlockReason::dx12_queue: return "dx12-queue";
    case BlockReason::dx12_owner: return "dx12-owner";
    case BlockReason::dx12_dispatch: return "dx12-dispatch";
    case BlockReason::dx12_presenter: return "dx12-presenter";
    case BlockReason::dx12_root: return "dx12-root";
    case BlockReason::dx12_shader: return "dx12-shader";
    case BlockReason::dx12_pipeline: return "dx12-pipeline";
    case BlockReason::dx12_commands: return "dx12-commands";
    case BlockReason::dx12_descriptors: return "dx12-descriptors";
    case BlockReason::dx12_constants: return "dx12-constants";
    }
    return "unknown";
}

inline const char* statusName(Status state) {
    switch (state) {
    case Status::off: return "off";
    case Status::waiting: return "waiting";
    case Status::unsupported: return "unsupported";
    case Status::unsupported_d3d12: return "unsupported-d3d12";
    case Status::active: return "active";
    }
    return "waiting";
}

// Config and heartbeat I/O stay off the application's frame loop and the
// native presenter. Each session owns a separate file, even in one process.
class SessionStatus {
    static inline std::atomic<unsigned> nextId{};
    std::atomic<bool> requested_{false};
    std::mutex mutex_;
    std::condition_variable changed_;
    bool stopping_{true};
    std::wstring path_, temporary_;
    std::thread worker_;

    bool publish(const std::string& record) const {
        const auto file = CreateFileW(temporary_.c_str(), GENERIC_WRITE, 0, nullptr,
            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) return false;
        DWORD written{};
        const bool ok = WriteFile(file, record.data(), static_cast<DWORD>(record.size()), &written, nullptr)
            && written == record.size();
        CloseHandle(file);
        return ok && MoveFileExW(temporary_.c_str(), path_.c_str(), MOVEFILE_REPLACE_EXISTING);
    }

public:
    ~SessionStatus() { stop(); }

    bool requested() const noexcept { return requested_.load(); }

    template<class Snapshot>
    void start(Snapshot snapshot) noexcept {
        if (worker_.joinable()) return;
        try {
            const auto pid = GetCurrentProcessId();
            path_ = L"C:\\FrameGen\\status\\" + std::to_wstring(pid)
                + L"-" + std::to_wstring(++nextId) + L".txt";
            temporary_ = path_ + L".tmp";
            auto name = executableName();
            { std::lock_guard lock(mutex_); stopping_ = false; }
            worker_ = std::thread([this, snapshot = std::move(snapshot), pid, name = std::move(name)] {
                try {
                    const auto prefix = std::to_string(pid) + '\t';
                const auto suffix = '\t' + name;
                std::optional<StatusSnapshot> previous;
                    ULONGLONG nextHeartbeat{};
                    for (;;) {
                        const auto enabled = readMode().value_or(false);
                        requested_ = enabled;
                        const auto now = GetTickCount64();
                        const StatusSnapshot state{snapshot(enabled, now)};
                        if (state != previous || now >= nextHeartbeat) {
                            auto record = prefix + statusName(state.state) + suffix;
                            if (state.issue.reason != BlockReason::none) {
                                record += '\t';
                                record += blockReasonName(state.issue.reason);
                                record += ':';
                                record += std::to_string(state.issue.detail);
                            }
                            if (publish(record + '\n')) previous = state;
                            nextHeartbeat = now + 500;
                        }
                        std::unique_lock lock(mutex_);
                        if (changed_.wait_for(lock, std::chrono::milliseconds(125), [this] { return stopping_; })) break;
                    }
                } catch (...) {
                    // Status reporting must never interfere with game execution.
                    requested_ = false;
                }
            });
        } catch (...) {
            stop();
        }
    }

    void stop() {
        { std::lock_guard lock(mutex_); stopping_ = true; }
        changed_.notify_one();
        if (worker_.joinable()) worker_.join();
        if (!temporary_.empty()) DeleteFileW(temporary_.c_str());
        if (!path_.empty()) DeleteFileW(path_.c_str());
        temporary_.clear();
        path_.clear();
        requested_ = false;
    }
};

inline bool isStudyLayer(std::string_view name) {
    return name == "XR_APILAYER_ARMAX_latency_study" ||
           name == "XR_APILAYER_ARMAX_metalfx_study" ||
           name == "XR_APILAYER_ARMAX_amd_color_study" ||
           name == "XR_APILAYER_ARMAX_amd_fixed_study";
}

inline bool supportsTransport(const XrInstanceCreateInfo& info) {
    for (uint32_t i = 0; i < info.enabledApiLayerCount; ++i)
        if (isStudyLayer(info.enabledApiLayerNames[i])) return false;
    char explicitLayers[2048]{};
    const auto length = GetEnvironmentVariableA("XR_ENABLE_API_LAYERS", explicitLayers, sizeof explicitLayers);
    if (length >= sizeof explicitLayers) return false;
    for (auto names = std::string_view(explicitLayers, length); !names.empty();) {
        const auto separator = names.find_first_of(";:");
        if (isStudyLayer(names.substr(0, separator))) return false;
        if (separator == std::string_view::npos) break;
        names.remove_prefix(separator + 1);
    }
    bool supportedGraphics = false;
    for (uint32_t i = 0; i < info.enabledExtensionCount; ++i) {
        const std::string_view name = info.enabledExtensionNames[i];
        if (name == "XR_KHR_D3D11_enable") supportedGraphics = true;
#ifdef FRAMEGEN_NATIVE_D3D12
        if (name == "XR_KHR_D3D12_enable") supportedGraphics = true;
#endif
        if (name == "XR_FB_space_warp" || name == "XR_FB_passthrough") return false;
        if (name.find("_composition_layer_") != std::string_view::npos &&
            name != "XR_KHR_composition_layer_depth" &&
            name != "XR_KHR_composition_layer_cube" &&
            name != "XR_KHR_composition_layer_cylinder" &&
            name != "XR_KHR_composition_layer_equirect" &&
            name != "XR_KHR_composition_layer_equirect2" &&
            name != "XR_KHR_composition_layer_color_scale_bias") return false;
    }
    return supportedGraphics;
}

// Limit the supported composition to one opaque projection and simple
// unchanged quads that the presenter can lease without borrowing XR structs.
inline const XrCompositionLayerProjection* checkStereoProjection(
    const XrFrameEndInfo* frame, AdmissionIssue& issue, bool allowQuadLayers = false,
    bool allowSharedProjectionSourceAlpha = false) {
    issue = {};
    if (!frame) return nullptr;
    if (frame->next) {
        issue = {BlockReason::frame_extensions, static_cast<const XrBaseInStructure*>(frame->next)->type};
        return nullptr;
    }
    if (frame->environmentBlendMode != XR_ENVIRONMENT_BLEND_MODE_OPAQUE) {
        issue = {BlockReason::blend_mode, frame->environmentBlendMode};
        return nullptr;
    }
    constexpr std::uint32_t maxSupportedLayers = 8;
    if (!frame->layerCount || frame->layerCount > maxSupportedLayers ||
        (!allowQuadLayers && frame->layerCount != 1) || !frame->layers) {
        issue = {BlockReason::layer_count, static_cast<std::int32_t>(frame->layerCount)};
        return nullptr;
    }
    const XrCompositionLayerBaseHeader* layer = nullptr;
    for (std::uint32_t index = 0; index < frame->layerCount; ++index) {
        const auto* candidate = frame->layers[index];
        if (candidate && candidate->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION && !layer) {
            layer = candidate;
            continue;
        }
        if (!candidate || candidate->type != XR_TYPE_COMPOSITION_LAYER_QUAD) {
            issue = {BlockReason::layer_type, candidate ? candidate->type : XR_TYPE_UNKNOWN};
            return nullptr;
        }
        if (const auto* extension = static_cast<const XrBaseInStructure*>(candidate->next)) {
            issue = {BlockReason::layer_extensions, extension->type};
            return nullptr;
        }
    }
    if (!layer) {
        issue = {BlockReason::layer_type, frame->layers[0]->type};
        return nullptr;
    }
    if (layer->next) {
        issue = {BlockReason::layer_extensions, static_cast<const XrBaseInStructure*>(layer->next)->type};
        return nullptr;
    }
    const auto* projection = reinterpret_cast<const XrCompositionLayerProjection*>(layer);
    if (projection->viewCount != 2 || !projection->views) {
        issue = {BlockReason::view_count, static_cast<std::int32_t>(projection->viewCount)};
        return nullptr;
    }
    // Without BLEND_TEXTURE_SOURCE_ALPHA the runtime uses alpha=1. The
    // fingerprinted MoltenVR stock path also ignores it on this projection.
    auto allowedFlags = XR_COMPOSITION_LAYER_CORRECT_CHROMATIC_ABERRATION_BIT |
        XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
    const auto& left = projection->views[0].subImage;
    const auto& right = projection->views[1].subImage;
    if (allowSharedProjectionSourceAlpha && frame->layerCount == 1 &&
        (layer->layerFlags & XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT) &&
        left.swapchain && left.swapchain == right.swapchain &&
        left.imageArrayIndex != right.imageArrayIndex) {
        allowedFlags |= XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    }
    if (layer->layerFlags & ~allowedFlags) {
        issue = {BlockReason::layer_flags, static_cast<std::int32_t>(layer->layerFlags)};
        return nullptr;
    }
    for (unsigned eye = 0; eye < 2; ++eye) {
        if (const auto* extension = static_cast<const XrBaseInStructure*>(projection->views[eye].next)) {
            issue = {BlockReason::view_extensions, extension->type};
            return nullptr;
        }
    }
    return projection;
}

inline bool checkColor(const XrSwapchainCreateInfo& chain, const XrSwapchainSubImage& sub, AdmissionIssue& issue) {
    if (chain.format != DXGI_FORMAT_B8G8R8A8_UNORM_SRGB &&
        chain.format != DXGI_FORMAT_R8G8B8A8_UNORM_SRGB &&
        chain.format != DXGI_FORMAT_B8G8R8A8_UNORM &&
        chain.format != DXGI_FORMAT_R8G8B8A8_UNORM) {
        issue = {BlockReason::color_format, static_cast<std::int32_t>(chain.format)};
        return false;
    }
    if (chain.sampleCount != 1) {
        issue = {BlockReason::color_samples, static_cast<std::int32_t>(chain.sampleCount)};
        return false;
    }
    if (chain.faceCount != 1 || !chain.mipCount || sub.imageArrayIndex >= chain.arraySize) {
        issue = {BlockReason::color_layout, static_cast<std::int32_t>(sub.imageArrayIndex)};
        return false;
    }
    const auto& r = sub.imageRect;
    const bool validRect = r.offset.x >= 0 && r.offset.y >= 0 &&
        r.extent.width >= 128 && r.extent.height >= 128 &&
        uint64_t(r.offset.x) + r.extent.width <= chain.width &&
        uint64_t(r.offset.y) + r.extent.height <= chain.height;
    if (!validRect) issue = {BlockReason::color_rect, r.extent.width};
    return validRect;
}


} // namespace runtime_study::menu_framegen
