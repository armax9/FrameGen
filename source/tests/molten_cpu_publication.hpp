#pragma once

#include <windows.h>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

// Writer for a caller-created, empty stand-alone fixture file. One publisher
// owns the inactive slot until both eyes and its source timestamp are ready.
class MoltenCpuPublication {
    static constexpr std::size_t headerSize = 4096;
    unsigned char* mapping_{};
    std::size_t eyeRowBytes_{}, rowBytes_{}, slotBytes_{};
    unsigned height_{}, activeSlot_{}, pendingSlot_{}, eyesWritten_{};
    bool pairOpen_{};

    void store32(std::size_t offset, std::uint32_t value) {
        InterlockedExchange(reinterpret_cast<volatile LONG*>(mapping_ + offset),
                            static_cast<LONG>(value));
    }

    void store64(std::size_t offset, std::uint64_t value) {
        InterlockedExchange64(reinterpret_cast<volatile LONG64*>(mapping_ + offset),
                              static_cast<LONG64>(value));
    }

public:
    MoltenCpuPublication() = default;
    MoltenCpuPublication(const MoltenCpuPublication&) = delete;
    MoltenCpuPublication& operator=(const MoltenCpuPublication&) = delete;

    ~MoltenCpuPublication() {
        if (mapping_) UnmapViewOfFile(mapping_);
    }

    [[nodiscard]] bool open(const wchar_t* file, unsigned eyeWidth, unsigned height,
                            std::uint32_t hostPid) {
        if (active() || !file || !*file || !eyeWidth || !height || !hostPid ||
            eyeWidth > std::numeric_limits<std::uint32_t>::max() / 2) return false;

        constexpr auto maxSlotBytes = (std::numeric_limits<std::size_t>::max() - headerSize) / 2;
        const std::size_t checkedWidth = eyeWidth;
        if (checkedWidth > maxSlotBytes / 8) return false;
        const auto rowBytes = checkedWidth * 8;
        if (height > maxSlotBytes / rowBytes) return false;
        const auto slotBytes = rowBytes * height;
        const auto fileBytes = headerSize + 2 * slotBytes;

        const HANDLE input = CreateFileW(file, GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (input == INVALID_HANDLE_VALUE) return false;
        LARGE_INTEGER length{};
        const bool exactSize = GetFileSizeEx(input, &length) && length.QuadPart >= 0 &&
            static_cast<std::uint64_t>(length.QuadPart) == fileBytes;
        const HANDLE mapping = exactSize
            ? CreateFileMappingW(input, nullptr, PAGE_READWRITE, 0, 0, nullptr)
            : nullptr;
        CloseHandle(input);
        if (!mapping) return false;
        auto* bytes = static_cast<unsigned char*>(MapViewOfFile(mapping, FILE_MAP_WRITE, 0, 0, fileBytes));
        CloseHandle(mapping);
        if (!bytes) return false;

        // A live or previously used producer header is ineligible. The
        // private helper may have populated its generation and surface IDs
        // at offsets 20 through 31 while waiting for the first frame.
        const unsigned char emptyHeader[64]{};
        if (std::memcmp(bytes, emptyHeader, 20) != 0 ||
            std::memcmp(bytes + 32, emptyHeader + 32, 32) != 0) {
            UnmapViewOfFile(bytes);
            return false;
        }

        mapping_ = bytes;
        eyeRowBytes_ = rowBytes / 2;
        rowBytes_ = rowBytes;
        slotBytes_ = slotBytes;
        height_ = height;
        store32(8, eyeWidth * 2);
        store32(12, height);
        store32(16, hostPid);
        store32(32, 0);
        return true;
    }

    [[nodiscard]] bool active() const { return mapping_ != nullptr; }

    [[nodiscard]] bool beginPair() {
        if (!active() || pairOpen_) return false;
        pairOpen_ = true;
        pendingSlot_ = 1 - activeSlot_;
        eyesWritten_ = 0;
        return true;
    }

    void writeEye(unsigned eye, const unsigned char* pixels, std::size_t rowPitch,
                  bool bgra) {
        if (!pairOpen_ || eye >= 2 || !pixels || rowPitch < eyeRowBytes_ ||
            (eyesWritten_ & (1u << eye))) return;

        auto* output = mapping_ + headerSize + pendingSlot_ * slotBytes_ + eye * eyeRowBytes_;
        for (unsigned row = 0; row < height_; ++row) {
            const auto* input = pixels + row * rowPitch;
            auto* destination = output + row * rowBytes_;
            if (bgra) {
                std::memcpy(destination, input, eyeRowBytes_);
            } else {
                for (std::size_t column = 0; column < eyeRowBytes_; column += 4) {
                    destination[column] = input[column + 2];
                    destination[column + 1] = input[column + 1];
                    destination[column + 2] = input[column];
                    destination[column + 3] = input[column + 3];
                }
            }
        }
        eyesWritten_ |= 1u << eye;
    }

    void commit(std::uint64_t tag, std::uint32_t fixtureMarker) {
        if (!pairOpen_) return;
        pairOpen_ = false;
        if (eyesWritten_ != 3 || !tag || !fixtureMarker) return;
        // One test pixel distinguishes a forecast from the real output
        // that carries the same source timestamp.
        store32(headerSize + (pendingSlot_ + 1) * slotBytes_ - 4,
                0xff000000U | fixtureMarker);
        store64(40 + pendingSlot_ * sizeof tag, tag);
        store32(0, pendingSlot_);
        store32(4, 1);
        activeSlot_ = pendingSlot_;
    }
};
