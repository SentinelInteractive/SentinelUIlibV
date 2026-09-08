// CEF host for the in-game overlay.
// Threading model (validated by the Phase-0 harness):
//   * ONE dedicated CEF thread: CefInitialize + CefDoMessageLoopWork pump.
//   * Browser is windowless OSR (transparent), --disable-gpu (software path).
//   * OnPaint (CEF thread) copies the BGRA frame into a mutex-guarded CPU buffer.
//   * The swapchain Present hook (game thread) uploads that buffer to the game's
//     D3D11 device texture and composites it. (renderer.cpp / hooks.cpp)

#pragma once
#ifndef SENTINEL_HOST_CEF_HOST_H
#define SENTINEL_HOST_CEF_HOST_H

#include <windows.h>
#include "include/cef_app.h"
#include "include/cef_browser.h"
#include "include/cef_client.h"
#include "include/cef_render_handler.h"
#include "include/cef_command_line.h"

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace sui_host {

// Absolute paths resolved at startup (module-relative), so CEF finds its runtime.
struct HostPaths {
    std::wstring moduleDir;      // .../SentinelUI (module directory)
    std::wstring contentFile;    // .../content/p1_demo.html (loaded at P1)
    std::wstring workerExe;      // .../SentinelUI_Worker.exe
    std::wstring cacheDir;       // .../cache
    std::wstring logDir;         // .../logs
    // CEF expects UTF-8/ASCII paths; keep narrow copies.
    std::string contentFileUtf8;
    std::string workerExeUtf8;
    std::string cacheDirUtf8;
};

struct Frame {
    std::vector<uint8_t> bgra;
    int width = 0;
    int height = 0;
    uint64_t generation = 0;   // incremented on every new frame
};

class CefHost {
public:
    bool Start(const HostPaths& paths);
    void Stop();                       // join the CEF thread, shutdown CEF

    // Game-thread side.
    bool TakeFrame(Frame& out);        // snapshot the latest frame if newer than lastSeen

    // CEF-thread side: publish a new OnPaint frame into the front buffer.
    void PublishFrame(const uint8_t* data, int width, int height);

    // WndProc side / managed calls.
    void SetVisible(bool visible);     // WasHidden + skip drawing
    bool visible() const { return visible_.load(); }
    void SetCapture(bool capture) { capture_.store(capture); }
    bool capture() const { return capture_.load(); }

    // Input (called from the WndProc chain on the game thread).
    void SendKey(bool down, UINT vk, UINT scancode, bool extended);
    void SendChar(wchar_t ch);
    void SendMouseMove(int x, int y);
    void SendMouseButton(int x, int y, int button, bool down, UINT flags);
    void SendMouseWheel(int x, int y, int delta);

    void ResizeView(int width, int height);   // on WM_SIZE

    const HostPaths& paths() const { return paths_; }
    bool started() const { return state_ == State::Running; }

private:
    enum class State { Idle, Running, Stopping, Stopped };
    State state_ = State::Idle;

    HostPaths paths_;
    std::atomic<bool> visible_{true};
    std::atomic<bool> capture_{false};
    std::atomic<int> viewWidth_{0};
    std::atomic<int> viewHeight_{0};

    std::thread thread_;               // dedicated CEF thread
    CefRefPtr<CefBrowser> browser_;
    std::atomic<bool> bVisible_{true}; // last WasHidden state we sent

    std::mutex frameMutex_;
    Frame front_;                      // latest OnPaint copy (CEF thread writes)
    uint64_t lastSeenGeneration_ = 0;  // game thread watermark

    void ThreadMain(const HostPaths& paths);
};

// Internal: dedicated CEF app instance. Constructed directly in cef_host.cpp (class SuiApp).
class CefHost;

}  // namespace sui_host

#endif