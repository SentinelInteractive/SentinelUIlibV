// SentinelUI ASI side — shared-memory frame consumer (mirror of helper's shm.h).

#pragma once
#ifndef SENTINEL_HOST_FRAMESRC_H
#define SENTINEL_HOST_FRAMESRC_H

#include <windows.h>
#include <cstdint>

namespace sui_host {

#pragma pack(push, 1)
struct ShmHeader {
    uint32_t magic;      // 'SUIF'
    uint32_t width;
    uint32_t height;
    uint64_t generation;
    uint64_t bytes;
    uint32_t state;
    uint32_t reserved;
};
#pragma pack(pop)

// Opens the named frame mapping written by SentinelUI_Host.exe.
class FrameSource {
public:
    bool Open(const wchar_t* name, uint32_t maxW, uint32_t maxH) {
        map_ = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, name);
        if (!map_) return false;
        base_ = (uint8_t*)MapViewOfFile(map_, FILE_MAP_ALL_ACCESS, 0, 0,
                                        sizeof(ShmHeader) + (size_t)maxW * maxH * 4);
        if (!base_) { CloseHandle(map_); map_ = nullptr; return false; }
        header_ = reinterpret_cast<ShmHeader*>(base_);
        return true;
    }

    // Returns true and fills data when a NEWER frame is available.
    bool Take(uint8_t* out, uint64_t maxBytes, uint64_t& gen) {
        if (!header_ || header_->magic != 0x46554953u) return false;
        uint64_t g = header_->generation;
        if (g == 0 || g == gen || header_->state != 1) return false;
        uint64_t bytes = header_->bytes;
        if (bytes > maxBytes) return false;
        memcpy(out, base_ + sizeof(ShmHeader), (size_t)bytes);
        gen = g;
        return true;
    }

    uint32_t width() const { return header_ ? header_->width : 0; }
    uint32_t height() const { return header_ ? header_->height : 0; }

    void Close() {
        if (base_) { UnmapViewOfFile(base_); base_ = nullptr; }
        if (map_) { CloseHandle(map_); map_ = nullptr; }
        header_ = nullptr;
    }

    ~FrameSource() { Close(); }

private:
    HANDLE map_ = nullptr;
    uint8_t* base_ = nullptr;
    ShmHeader* header_ = nullptr;
};

}  // namespace sui_host

#endif