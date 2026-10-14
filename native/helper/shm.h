// SentinelUI Helper: shared-memory frame transport.
// The EXE-hosted CEF process publishes BGRA frames here; the in-game ASI
// compositor maps the same named object and uploads them to the D3D11 texture.

#pragma once

#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace sui_helper {

#pragma pack(push, 1)
struct FrameHeader {
    uint32_t magic;      // 'SUIF'
    uint32_t width;
    uint32_t height;
    uint64_t generation;
    uint64_t bytes;      // width*height*4
    uint32_t state;      // 0 = empty, 1 = ready
    uint32_t reserved;
};
#pragma pack(pop)

class FrameShm {
public:
    // Creates (or opens) a named shared-memory mapping sized for maxW*maxH BGRA.
    bool Open(const wchar_t* name, uint32_t maxW, uint32_t maxH) {
        DWORD mapSize = (DWORD)(sizeof(FrameHeader) + (size_t)maxW * maxH * 4);
        map_ = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, mapSize, name);
        if (!map_) return false;
        base_ = (uint8_t*)MapViewOfFile(map_, FILE_MAP_ALL_ACCESS, 0, 0, mapSize);
        if (!base_) { CloseHandle(map_); map_ = nullptr; return false; }
        cap_ = mapSize;
        header_ = reinterpret_cast<FrameHeader*>(base_);
        if (header_->magic != 0x46554953u) {
            memset(base_, 0, sizeof(FrameHeader));
        }
        header_->magic = 0x46554953u;
        header_->state = 0;
        return true;
    }

    void Publish(const uint8_t* bgra, uint32_t w, uint32_t h) {
        if (!base_) return;
        size_t bytes = (size_t)w * h * 4;
        if (sizeof(FrameHeader) + bytes > cap_) { bytes = cap_ - sizeof(FrameHeader); }
        if (header_->bytes != bytes || header_->width != w || header_->height != h) {
            // Viewport changed: reset layout.
            header_->width = w;
            header_->height = h;
            header_->bytes = (uint64_t)bytes;
        }
        memcpy(base_ + sizeof(FrameHeader), bgra, bytes);
        header_->state = 1;
        header_->generation++;
    }

    const FrameHeader* Header() const { return header_; }
    const uint8_t* Pixels() const { return base_ ? base_ + sizeof(FrameHeader) : nullptr; }

    void Close() {
        if (base_) { UnmapViewOfFile(base_); base_ = nullptr; }
        if (map_) { CloseHandle(map_); map_ = nullptr; }
        header_ = nullptr;
    }

    ~FrameShm() { Close(); }

private:
    HANDLE map_ = nullptr;
    uint8_t* base_ = nullptr;
    size_t cap_ = 0;
    FrameHeader* header_ = nullptr;
};

}  // namespace sui_helper