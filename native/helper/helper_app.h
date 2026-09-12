// SentinelUI Helper — CEF host inside a dedicated EXE process.
// Proven harness pattern: dedicated CEF thread + CefDoMessageLoopWork pump,
// transparent OSR with --disable-gpu, OnPaint -> per-layer shared-memory frame.
//
// Issue #2 layer model: every UI layer owns a CefBrowser + a named shared
// memory mapping (layer 1 = the boot page; ids >= 2 are created on demand by
// the ASI via the 'C' command). The ASI composites the per-layer frames in
// z order. Layer operations arriving on the IPC reader thread are queued and
// executed on the CEF UI (pump) thread.

#pragma once
#ifndef SENTINEL_HELPER_APP_H
#define SENTINEL_HELPER_APP_H

#include <windows.h>

#include "include/cef_app.h"
#include "include/cef_browser.h"
#include "include/cef_client.h"
#include "include/cef_frame.h"
#include "include/cef_render_handler.h"
#include "include/cef_v8.h"
#include "include/cef_command_line.h"
#include "include/internal/cef_types_wrappers.h"

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "ipc.h"
#include "scheme.h"
#include "shm.h"
#include "suilog.h"

namespace sui_helper {

// Software cursor injected into pages (defined in helper_app.cpp).
extern const char* kCursorJs;
// Protocol bridge (`window.sui`: send/on/request) injected into every page.
extern const char* kSuiJs;
// Capture badge state toggles (defined in helper_app.cpp).
extern const char* kCaptureStateJsOn;
extern const char* kCaptureStateJsOff;

struct HelperConfig {
    std::wstring moduleDir;      // exe directory
    std::wstring contentFile;    // boot-layer URL (sentinelui://... or file path)
    std::wstring cacheDir;
    std::wstring logDir;
    std::wstring shmName;        // L"Local\\SentinelUI_Frames" etc. (layer 1 name base)
    std::wstring pipeName;       // L"\\\\.\\pipe\\SentinelUI"
    int viewWidth = 1280;
    int viewHeight = 720;
    int selftestPaints = 12;
    int timeoutSeconds = 0;      // 0 = run forever (until Shutdown)
    bool selftest = false;
    unsigned long parentPid = 0; // game process pid (fast orphan cleanup)
};

// ---------------------------------------------------------------------------
// One UI layer: CefClient + render/display/load handlers + frame mapping.
// All CEF-touching methods MUST run on the CEF UI (pump) thread.
// ---------------------------------------------------------------------------
class LayerClient : public CefClient,
                    public CefRenderHandler,
                    public CefLifeSpanHandler,
                    public CefDisplayHandler,
                    public CefLoadHandler,
                    public CefRequestHandler {
public:
    static constexpr uint32_t kMaxFrameW = 4096;
    static constexpr uint32_t kMaxFrameH = 2160;

    explicit LayerClient(int layerId);
    ~LayerClient() override;

    int layerId() const { return layerId_; }
    CefRefPtr<CefBrowser> browser() const { return browser_; }
    FrameShm* shm() { return &shm_; }
    void SetBrowser(CefRefPtr<CefBrowser> b) { browser_ = b; }  // UI thread

    // Pre-creation viewport (so a layer created after the game window is known
    // is born at the game resolution instead of the 1280x720 default).
    void SetInitialViewport(int width, int height) {
        if (width > 0 && height > 0) { viewWidth_ = width; viewHeight_ = height; }
    }

    // Creates/opens this layer's shared-memory mapping. Call before creating
    // the browser (the ASI may already be waiting on the mapping).
    bool OpenShm();

    // CefClient
    CefRefPtr<CefRenderHandler> GetRenderHandler() override { return this; }
    CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override { return this; }
    CefRefPtr<CefDisplayHandler> GetDisplayHandler() override { return this; }
    CefRefPtr<CefLoadHandler> GetLoadHandler() override { return this; }
    CefRefPtr<CefRequestHandler> GetRequestHandler() override { return this; }

    // CefRequestHandler — request diagnostics (one log line per URL prefix).
    CefRefPtr<CefResourceRequestHandler> GetResourceRequestHandler(
        CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>, CefRefPtr<CefRequest> request,
        bool is_navigation, bool, const CefString&, bool&) override {
        static std::set<std::string> s_seen;
        std::string url = request->GetURL().ToString();
        if (s_seen.insert(url.substr(0, 96)).second)
            SUI_LOG_INFO("req: %s (nav=%d)", url.c_str(), is_navigation ? 1 : 0);
        return nullptr;   // default handling (scheme factory)
    }

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
        // While hidden (F9 master or per-layer), CEF may emit an empty frame as
        // part of WasHidden. Publishing it would blank the layer for up to a
        // repaint interval after the next show, because the host composites the
        // last resident frame instantly. Keep the last GOOD frame instead.
        if (!visible_ || !masterVisible_) {
            if (!ignoreLog_) {
                ignoreLog_ = true;
                SUI_LOG_INFO("layer %d: paint ignored while hidden (last frame kept).", layerId_);
            }
            return;
        }
        ignoreLog_ = false;
        viewWidth_ = w;
        viewHeight_ = h;
        shm_.Publish((const uint8_t*)buffer, (uint32_t)w, (uint32_t)h);
        paintCount++;
    }

    int paintCount = 0;

    // CefLifeSpanHandler
    void OnAfterCreated(CefRefPtr<CefBrowser> browser) override;
    void OnBeforeClose(CefRefPtr<CefBrowser> browser) override;

    // CefDisplayHandler — JS->host channel (console messages, layer-tagged).
    bool OnConsoleMessage(CefRefPtr<CefBrowser>, cef_log_severity_t, const CefString& message,
                          const CefString&, int) override;

    // CefLoadHandler — load state events + software cursor injection.
    void OnLoadingStateChange(CefRefPtr<CefBrowser>, bool isLoading, bool, bool) override;

    void OnLoadError(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>, ErrorCode code,
                     const CefString& text, const CefString& url) override;

    // ---- commands (UI thread) ----
    void ExecuteJs(const std::string& javascript);
    void SetVisible(bool visible);
    void SetMasterVisible(bool visible);
    void SetCaptureIndicator(bool on);
    void SetFocused(bool keyboard, bool mouse);
    void SetViewport(int width, int height);
    void DispatchKey(bool down, int vk, int scancode, bool extended, int modifiers);
    void DispatchChar(int codePoint);
    void DispatchMouseMove(int x, int y);
    void DispatchMouseButton(int button, bool down, int x, int y);
    void DispatchMouseWheel(int delta, int x, int y);
    void Navigate(const std::string& urlUtf8);
    void Reload();

    // Queue JS for execution on the CEF pump thread (thread-safe).
    void QueueJs(const std::string& javascript);
    // Executes queued JS on the pump thread.
    void FlushJsQueue();
    // Pump-thread housekeeping: fires a pending navigation retry (see OnLoadError).
    void Tick();

private:
    void LogConsole(const std::string& msg);
    // Layer-tagged event to the ASI: "<id>|<kind>:<body>".
    void SendEvent(const std::string& kind, const std::string& body);
    // WasHidden(visible_ && master_visible_) + repaint on show.
    void ApplyVisibility();

    int layerId_;
    FrameShm shm_;
    CefRefPtr<CefBrowser> browser_;
    int viewWidth_ = 1280;
    int viewHeight_ = 720;
    bool visible_ = true;         // per-layer visibility (SuiSetLayerVisible)
    bool masterVisible_ = true;   // F9 master pause/resume
    bool captureOn_ = false;      // input capture badge state (queued to pages)
    bool ignoreLog_ = false;      // one-shot "paint ignored while hidden" log
    std::atomic<bool> jsReady_{false};   // set on load commit (or error page)
    std::mutex jsMutex_;
    std::vector<std::string> jsQueue_;

    // First-navigation race insurance: CEF child processes (network service)
    // register custom schemes at startup; under heavy load (game boot) the
    // first sentinelui:// request can beat that registration and fail with
    // ERR_UNKNOWN_URL_SCHEME. Retry a bounded number of times.
    static constexpr int kMaxLoadRetries = 5;
    int loadRetries_ = 0;
    bool retryPending_ = false;
    uint64_t retryAtTick_ = 0;
    std::string retryUrl_;
    IMPLEMENT_REFCOUNTING(LayerClient);
};

// ---------------------------------------------------------------------------
// CefApp (all processes) + browser/render process handlers.
// ---------------------------------------------------------------------------
class SuiHelperApp : public CefApp,
                     public CefBrowserProcessHandler,
                     public CefRenderProcessHandler {
public:
    // CefApp
    void OnBeforeCommandLineProcessing(const CefString&,
                                       CefRefPtr<CefCommandLine> command_line) override {
        command_line->AppendSwitch("disable-gpu");
        command_line->AppendSwitch("disable-gpu-compositing");
    }

    // Register sentinelui:// as a STANDARD custom scheme in every process
    // (host+path URLs, CSP, same-origin rules). LOCAL keeps the same-origin
    // security posture of file://-style content; SECURE avoids insecure-origin
    // downgrades.
    void OnRegisterCustomSchemes(CefRawPtr<CefSchemeRegistrar> registrar) override {
        bool ok = registrar->AddCustomScheme("sentinelui", CEF_SCHEME_OPTION_STANDARD |
                                                                  CEF_SCHEME_OPTION_SECURE |
                                                                  CEF_SCHEME_OPTION_CORS_ENABLED);
        LogSchemeRegistration(ok);
    }

    CefRefPtr<CefBrowserProcessHandler> GetBrowserProcessHandler() override { return this; }
    CefRefPtr<CefRenderProcessHandler> GetRenderProcessHandler() override { return this; }

    // Renderer process: inject the window.sui bridge into EVERY frame context
    // before any page script runs. The browser-process post-load injection alone
    // raced page scripts (inline <script>s executed while window.sui was absent —
    // caught by the HostProbe request test 2026-09-12).
    void OnContextCreated(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
                          CefRefPtr<CefV8Context> context) override {
        (void)browser;
        (void)context;
        if (frame && frame->IsMain()) frame->ExecuteJavaScript(kSuiJs, "sui://bridge", 0);
    }

    // Browser process: register the sentinelui:// scheme handler factory on
    // the IO thread (must precede any layer navigation).
    void OnContextInitialized() override;

private:
    IMPLEMENT_REFCOUNTING(SuiHelperApp);
};

// ---------------------------------------------------------------------------
// Layer manager: owns all LayerClients. Public ops are thread-safe — they are
// queued and executed on the CEF UI (pump) thread via PumpOps().
// ---------------------------------------------------------------------------
class LayerManager {
public:
    static LayerManager& Instance();

    void SetShmBaseName(const std::wstring& base) { shmBase_ = base; }
    const std::wstring& ShmBaseName() const { return shmBase_; }

    // UI thread only: create + open the browser right now (layer 1 bootstrap).
    bool CreateLayerNow(int layerId, const std::string& urlUtf8);

    // Thread-safe queued ops (from the IPC reader thread / exports).
    void CreateLayer(int layerId, const std::string& urlUtf8);
    void CloseLayer(int layerId);
    void SetVisible(int layerId, bool visible);
    void SetMasterVisible(bool visible);   // F9: pause/resume every page
    void SetCaptureIndicator(bool on);     // input-capture badge on every page
    void SetFocus(int layerId, bool keyboard, bool mouse);
    void ExecuteJs(int layerId, const std::string& js);
    void Navigate(int layerId, const std::string& urlUtf8);
    void Reload(int layerId);
    void SetViewportAll(int width, int height);   // global viewport sync
    void DispatchKey(int layerId, bool down, int vk, int scancode, bool extended, int modifiers);
    void DispatchChar(int layerId, int codePoint);
    void DispatchMouseMove(int layerId, int x, int y);
    void DispatchMouseButton(int layerId, int button, bool down, int x, int y);
    void DispatchMouseWheel(int layerId, int delta, int x, int y);

    // Pump loop entry points (UI thread).
    void PumpOps();
    void FlushJsQueues();
    void Tick();                      // per-pump housekeeping (retries, ...)
    void CloseAll();                  // graceful shutdown (UI thread)
    bool AnyBrowserAlive() const;     // UI thread
    LayerClient* Get(int layerId) const;  // UI thread
    int focusedId() const { return focusedId_; }

    // UI thread (from LayerClient::OnBeforeClose).
    void OnLayerBrowserClosed(int layerId);

private:
    LayerManager() = default;

    std::wstring shmBase_ = L"Local\\SentinelUI_Frames";
    int lastViewportW_ = 0;   // last game viewport (UI thread); new layers adopt it
    int lastViewportH_ = 0;
    bool masterVisible_ = true;   // F9 state (UI thread); adopted by new layers
    bool captureOn_ = false;      // capture badge state; adopted by new layers
    std::mutex opsMutex_;
    std::vector<std::function<void()>> ops_;  // queued layer ops
    std::map<int, CefRefPtr<LayerClient>> layers_;  // UI thread
    int focusedId_ = 0;
};

// Runs the CEF browser-process main loop on the calling thread (blocking until
// Shutdown). Returns after CefShutdown.
int RunBrowserProcess(const HelperConfig& cfg);

// Registers the IPC command handler (call after StartIpc).
void SetupIpcCommands();

// Parent-process watchdog (helper fast-exit): waits on the game process passed
// via --parent-pid and reports once it exits, so the pump loop can quit
// immediately instead of relying on the 30s pipe-down orphan guard.
void StartParentWatchdog(unsigned long parentPid);
bool ParentProcessGone();

}  // namespace sui_helper

#endif
