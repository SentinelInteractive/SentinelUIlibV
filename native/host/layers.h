// SentinelUI ASI — layer stack (issue #2).
//
// The host owns the layer model: every plugin UI surface is one layer with an
// id, z-order, visibility, click-through flag and (exclusive) input focus. The
// helper process renders each layer with its own CefBrowser and publishes its
// frames to a per-layer shared-memory mapping; this table pairs those mappings
// with the compositing/input state.
//
// Ids: 1 is the boot layer (created by the helper from --content at startup);
// SuiCreateLayer assigns ids >= 2. Shared-memory names: the base name for
// layer 1, "<base>_L<id>" for every other layer.
//
// Threading: exports + Present/WndProc all run on the game thread, but keep
// the internal mutex anyway (cheap, and the table also feeds the cursor-poll
// thread for input routing).

#pragma once
#ifndef SENTINEL_HOST_LAYERS_H
#define SENTINEL_HOST_LAYERS_H

#include "framesrc.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace sui_host {

class LayerTable {
public:
    static constexpr int kBaseLayerId = 1;
    static constexpr int kMaxLayers = 8;       // P5 budget: <=3 visible at once
    static constexpr uint32_t kMaxFrameW = 4096;
    static constexpr uint32_t kMaxFrameH = 2160;

    LayerTable() = default;
    LayerTable(const LayerTable&) = delete;
    LayerTable& operator=(const LayerTable&) = delete;

    void SetShmBaseName(const std::wstring& base) { shmBase_ = base; }

    // Registers the boot layer (id 1). Its browser already exists helper-side
    // (created from --content), so no 'C' command is sent.
    void CreateBase(const std::string& urlUtf8);

    // Creates a layer (sends the 'C' command). Returns the id, 0 on failure.
    int Create(const std::string& urlUtf8);

    // Closes a layer (sends 'X', drops the frame source). No-op when unknown.
    void Close(int id);
    void CloseAll();

    bool Exists(int id);

    // Snapshot of the layer's scalar state. Returns false when unknown.
    struct Info {
        int id = 0;
        int z = 0;
        bool visible = false;
        bool clickThrough = false;
        bool focused = false;
        std::string url;
    };
    bool Get(int id, Info& out);

    void SetVisible(int id, bool visible);
    void SetZ(int id, int z);
    void SetClickThrough(int id, bool enabled);
    // Exclusive focus; id <= 0 clears focus. Sends the 'F' command.
    void SetFocus(int id, bool keyboard, bool mouse);

    // Visible layer ids in compositing order: ascending z, ties by id.
    std::vector<int> DrawOrder();

    // Input routing (capture mode):
    //   mouse -> topmost visible layer that is NOT click-through.
    //   keys  -> focused layer, else the mouse target.
    // Returns 0 when nothing accepts input.
    int MouseTarget();
    int KeyTarget();

    // Present-side pump (game thread): lazily opens pending per-layer frame
    // mappings (rate-limited) and fetches every layer's newest frame into
    // buf (sequentially reused). Returns the ids that produced a new frame,
    // with the frame size of each. |force| re-takes the latest frame for every
    // layer even when no new generation arrived (used when the master overlay
    // becomes visible so all layers composite in the same frame).
    struct FrameInfo {
        int id = 0;
        uint32_t width = 0;
        uint32_t height = 0;
    };
    std::vector<FrameInfo> PumpFrames(uint8_t* buf, size_t bufBytes, bool force = false);

    size_t Count();

private:
    struct Layer {
        int id = 0;
        std::string url;
        int z = 0;
        bool visible = true;
        bool clickThrough = false;
        bool focused = false;
        FrameSource frames;
        bool framesOpen = false;
        uint64_t lastGen = 0;
        uint64_t nextOpenAttemptTick = 0;   // GetTickCount64 gate for retry
    };

    Layer* FindLocked(int id);
    std::wstring ShmNameLocked(int id) const;
    bool OpenFramesLocked(Layer& l);

    std::mutex mu_;
    std::vector<std::unique_ptr<Layer>> layers_;
    std::wstring shmBase_ = L"Local\\SentinelUI_Frames";
    int nextId_ = 2;   // 1 = boot layer
};

}  // namespace sui_host

#endif
