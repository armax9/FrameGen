#pragma once

#include "xrfg/private_openxr_runtime.hpp"
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <type_traits>

namespace runtime_study {
// One writer (the native presenter). A shared mapping keeps file I/O off that
// thread and retains completed records if the test runner terminates Wine.
class PublicationTimingCapture {
    using Timing = xrfg::PrivateOpenXrRuntime::Options::PublicationTiming;

    struct Header {
        char magic[8];
        std::uint64_t capacity;
        std::uint64_t record_size;
        volatile LONG64 written;
        std::int64_t qpc_anchor_ns, unix_anchor_ns, anchor_span_ns;
    };
    static constexpr std::uint64_t capacity = 8192;
    static_assert(std::is_standard_layout_v<Timing>);
    static constexpr std::size_t recordV1 = sizeof(LONG64) + offsetof(Timing, application);
    static constexpr std::size_t recordV2 = sizeof(LONG64) + offsetof(Timing, preparation);
    static constexpr std::size_t recordV3 = sizeof(LONG64) + offsetof(Timing, app_stages);
    static constexpr std::size_t recordV4 = sizeof(LONG64) + sizeof(Timing);
    static_assert(sizeof(Header) == 56 && recordV1 == 112 && recordV2 == 144 && recordV3 == 232 && recordV4 == 248);

    HANDLE file_ = INVALID_HANDLE_VALUE;
    Header* mapped_ = nullptr;
    std::size_t recordSize_ = recordV1;
    LONG64 written_ = 0;

    static bool outputPath(wchar_t (&path)[MAX_PATH]) noexcept {
        HMODULE module = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(&outputPath), &module)) return false;
        const auto size = GetModuleFileNameW(module, path, MAX_PATH);
        if (!size || size >= MAX_PATH) return false;
        auto* slash = std::wcsrchr(path, L'\\');
        if (!slash) return false;
        return wcscpy_s(slash + 1, MAX_PATH - (slash + 1 - path),
                       L"MoltenVR_Publication_Timing.bin") == 0;
    }

    void calibrate() noexcept {
        constexpr std::uint64_t epoch = 116444736000000000ULL;
        LARGE_INTEGER frequency{};
        QueryPerformanceFrequency(&frequency);
        const auto nanoseconds = [frequency](LARGE_INTEGER value) {
            return value.QuadPart / frequency.QuadPart * 1'000'000'000LL +
                value.QuadPart % frequency.QuadPart * 1'000'000'000LL / frequency.QuadPart;
        };
        auto& header = *mapped_;
        header.anchor_span_ns = INT64_MAX;
        for (int sample = 0; sample < 8; ++sample) {
            LARGE_INTEGER start{}, finish{};
            FILETIME wall{};
            QueryPerformanceCounter(&start);
            GetSystemTimePreciseAsFileTime(&wall);
            QueryPerformanceCounter(&finish);
            const auto span = nanoseconds(finish) - nanoseconds(start);
            if (span >= header.anchor_span_ns) continue;
            const auto ticks = (std::uint64_t{wall.dwHighDateTime} << 32) | wall.dwLowDateTime;
            header.anchor_span_ns = span;
            header.qpc_anchor_ns = nanoseconds(start) + span / 2;
            header.unix_anchor_ns = static_cast<std::int64_t>(ticks - epoch) * 100;
        }
    }

public:
    PublicationTimingCapture() = default;
    PublicationTimingCapture(const PublicationTimingCapture&) = delete;
    PublicationTimingCapture& operator=(const PublicationTimingCapture&) = delete;
    ~PublicationTimingCapture() {
        if (mapped_) UnmapViewOfFile(mapped_);
        if (file_ != INVALID_HANDLE_VALUE) CloseHandle(file_);
    }

    bool open(bool applicationRelease = false, bool realPreparation = false, bool applicationStages = false) noexcept {
        wchar_t path[MAX_PATH]{};
        if (!outputPath(path)) return false;
        file_ = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ,
                            nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file_ == INVALID_HANDLE_VALUE) return false;
        recordSize_ = applicationStages ? recordV4 : realPreparation ? recordV3 : applicationRelease ? recordV2 : recordV1;
        const auto bytes = sizeof(Header) + recordSize_ * capacity;
        const auto section = CreateFileMappingW(file_, nullptr, PAGE_READWRITE, 0, static_cast<DWORD>(bytes), nullptr);
        if (!section) return false;
        mapped_ = static_cast<Header*>(MapViewOfFile(section, FILE_MAP_WRITE, 0, 0, bytes));
        CloseHandle(section);
        if (!mapped_) return false;
        // Pre-touch pages during setup instead of faulting them in mid-frame.
        std::memset(mapped_, 0, bytes);
        mapped_->capacity = capacity;
        mapped_->record_size = recordSize_;
        calibrate();
        const auto* magic = applicationStages ? "MVRPUB4" : realPreparation ? "MVRPUB3" :
            applicationRelease ? "MVRPUB2" : "MVRPUB1";
        std::memcpy(mapped_->magic, magic, sizeof(mapped_->magic));
        return true;
    }

    void observe(const Timing& timing) noexcept {
        const auto ordinal = ++written_;
        auto* record = reinterpret_cast<unsigned char*>(mapped_) + sizeof(Header) +
                       ((ordinal - 1) % capacity) * recordSize_;
        auto* committed = reinterpret_cast<volatile LONG64*>(record);
        InterlockedExchange64(committed, 0);
        std::memcpy(record + sizeof(LONG64), &timing, recordSize_ - sizeof(LONG64));
        InterlockedExchange64(committed, ordinal);
        InterlockedExchange64(&mapped_->written, ordinal);
    }
};
} // namespace runtime_study
