// SentinelUI Helper — CEF host inside a dedicated EXE process.
// Proven harness pattern: dedicated CEF thread + CefDoMessageLoopWork pump,
// transparent OSR with --disable-gpu, OnPaint -> shared-memory frame.

#pragma once
#ifndef SENTINEL_HELPER_APP_H
#define SENTINEL_HELPER_APP_H

#include <windows.h>

#include "include/cef_app.h"
#include "include/cef_browser.h"
#include "include/cef_client.h"
#include "include/cef_render_handler.h"
#include "include/cef_command_line.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "ipc.h"
#include "shm.h"

namespace sui_helper {

// Software cursor injected into pages (defined in helper_app.cpp).
extern const char* kCursorJs;

struct HelperConfig {
    std::wstring moduleDir;      // exe directory
    std::wstring contentFile;    // file:// URL target
    std::wstring cacheDir;
    std::wstring logDir;
    std::wstring shmName;        // L"Local\\SentinelUI_Frames" etc.
    std::wstring pipeName;       // L"\\\\.\\pipe\\SentinelUI"
    int viewWidth = 1280;
    int viewHeight = 720;
    int selftestPaints = 12;
    int timeoutSeconds = 0;      // 0 = run forever (until Shutdown)
    bool selftest = false;
};

// The reference-counted CEF app; lives on the dedicated thread.
class SuiHelperApp : public CefApp,
                     public CefClient,
                     public CefRenderHandler,
                     public CefLifeSpanHandler,
                     public CefDisplayHandler,
                     public CefLoadHandler {
public:
    SuiHelperApp(FrameShm* shm) : shm_(shm), paintCount(0), viewWidth_(1280), viewHeight_(720) {}

    // CefApp
    void OnBeforeCommandLineProcessing(const CefString&,
                                       CefRefPtr<CefCommandLine> command_line) override {
        command_line->AppendSwitch("disable-gpu");
        command_line->AppendSwitch("disable-gpu-compositing");
    }

    // CefClient
    CefRefPtr<CefRenderHandler> GetRenderHandler() override { return this; }
    CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override { return this; }
    CefRefPtr<CefDisplayHandler> GetDisplayHandler() override { return this; }
    CefRefPtr<CefLoadHandler> GetLoadHandler() override { return this; }

    // CefRenderHandler
    void GetViewRect(CefRefPtr<CefBrowser>, CefRect& rect) override {
        rect = CefRect(0, 0, viewWidth_, viewHeight_);
    }

    bool GetScreenInfo(CefRefPtr<CefBrowser>, CefScreenInfo& screen) override {
        screen.device_scale_factor = 1.0f;
        screen.rect = CefRect(0, 0, viewWidth_, viewHeight_);
        screen.available_rect = screen.rect;
        return true;
    }

    void OnPaint(CefRefPtr<CefBrowser>, PaintElementType type, const RectList&,
                 const void* buffer, int w, int h) override {
        if (type != PET_VIEW) return;
        viewWidth_ = w;
        viewHeight_ = h;
        shm_->Publish((const uint8_t*)buffer, (uint32_t)w, (uint32_t)h);
        paintCount++;
    }

    int paintCount = 0;

    // CefLifeSpanHandler
    void OnAfterCreated(CefRefPtr<CefBrowser> browser) override {
        if (!browser->IsPopup()) browser_ = browser;
    }

    // CefDisplayHandler
    bool OnConsoleMessage(CefRefPtr<CefBrowser>, cef_log_severity_t, const CefString& message,
                          const CefString&, int) override {
        // JS->host channel: log locally + forward to the ASI (issue #1 bridge).
        std::string msg = message.ToString();
        LogConsole(msg);
        IpcSendEvent("console:" + msg);
        return true;
    }

    // CefLoadHandler
    void OnLoadingStateChange(CefRefPtr<CefBrowser>, bool isLoading, bool, bool) override {
        LogConsole(isLoading ? std::string("[load] loading") : std::string("[load] done"));
        IpcSendEvent(isLoading ? "load:loading" : "load:done");
        if (!isLoading) {
            // Inject the software cursor once the page finished loading.
            QueueJs(kCursorJs);
        }
    }

    void OnLoadError(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>, ErrorCode code,
                     const CefString& text, const CefString& url) override {
        std::string body = text.ToString() + " url=" + url.ToString();
        LogConsole("[load-error] " + body);
        IpcSendEvent("load-error:" + body);
    }

    CefRefPtr<CefBrowser> browser() const { return browser_; }
    void SetProcessor(void (*fn)(const std::string&)) { logFn_ = fn; }

    // Queue JS for execution on the CEF pump thread (thread-safe).
    void QueueJs(const std::string& javascript);

    // Executes queued JS on the pump thread.
    void FlushJsQueue();

    // ---- commands from the ASI (game thread -> pipe -> here) ----
    void ExecuteJs(const std::string& javascript);
    void SetVisible(bool visible);
    void SetFocused(bool keyboard, bool mouse);
    void SetViewport(int width, int height);
    void DispatchKey(bool down, int vk, int scancode, bool extended);
    void DispatchChar(int codePoint);
    void DispatchMouseMove(int x, int y);
    void DispatchMouseButton(int button, bool down, int x, int y);
    void DispatchMouseWheel(int delta, int x, int y);
    void Navigate(const std::string& urlUtf8);

private:
    void LogConsole(const std::string& msg) {
        if (logFn_) logFn_(msg);
    }

    std::mutex jsMutex_;
    std::vector<std::string> jsQueue_;

    FrameShm* shm_;
    CefRefPtr<CefBrowser> browser_;
    int viewWidth_;
    int viewHeight_;
    void (*logFn_)(const std::string&) = nullptr;
    IMPLEMENT_REFCOUNTING(SuiHelperApp);
};

// Runs the CEF browser-process main loop on the calling thread (blocking until
// Shutdown). Returns after CefShutdown.
int RunBrowserProcess(const HelperConfig& cfg, FrameShm* shm);

// Registers the IPC command handler (call after StartIpc).
void SetupIpcCommands();

}  // namespace sui_helper

#endif