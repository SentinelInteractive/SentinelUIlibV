#include "cef_host.h"
#include "suilog.h"

#include "include/cef_browser.h"
#include "include/cef_v8.h"

#include <cstdio>
#include <cwchar>
#include <string>
#include <thread>

namespace sui_host {

// Reference-counted CEF app living on the dedicated CEF thread. Implements all
// interfaces the host needs (CefApp for switch control, CefClient sinks for
// rendering/lifecycle/display/load). Defined here (non-anonymous) so the host
// can upcast to both CefApp and CefClient from the same instance.
class SuiApp : public CefApp,
               public CefClient,
               public CefRenderHandler,
               public CefLifeSpanHandler,
               public CefDisplayHandler,
               public CefLoadHandler {
public:
    explicit SuiApp(CefHost* host) : host_(host), width_(0), height_(0) {}

    // ---- CefApp ----
    void OnBeforeCommandLineProcessing(const CefString& process_type,
                                       CefRefPtr<CefCommandLine> command_line) override {
        // Software OSR path (validated in the Phase-0 harness). GPU-process
        // rendering is not production-ready for partial OSR updates.
        command_line->AppendSwitch("disable-gpu");
        command_line->AppendSwitch("disable-gpu-compositing");
    }

    // ---- CefClient ----
    CefRefPtr<CefRenderHandler> GetRenderHandler() override { return this; }
    CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override { return this; }
    CefRefPtr<CefDisplayHandler> GetDisplayHandler() override { return this; }
    CefRefPtr<CefLoadHandler> GetLoadHandler() override { return this; }

    // ---- CefRenderHandler ----
    void GetViewRect(CefRefPtr<CefBrowser>, CefRect& rect) override {
        if (width_ <= 0) width_ = 1280;
        if (height_ <= 0) height_ = 720;
        rect = CefRect(0, 0, width_, height_);
    }

    bool GetScreenInfo(CefRefPtr<CefBrowser>, CefScreenInfo& screen) override {
        screen.device_scale_factor = 1.0f;
        screen.rect = CefRect(0, 0, width_ <= 0 ? 1280 : width_, height_ <= 0 ? 720 : height_);
        screen.available_rect = screen.rect;
        return true;
    }

    void OnPaint(CefRefPtr<CefBrowser>, PaintElementType type, const RectList&,
                 const void* buffer, int w, int h) override {
        if (type != PET_VIEW) return;
        width_ = w;
        height_ = h;
        host_->PublishFrame((const uint8_t*)buffer, w, h);
    }

    // ---- CefLifeSpanHandler ----
    void OnAfterCreated(CefRefPtr<CefBrowser> browser) override {
        if (browser->IsPopup()) return;
        SUI_LOG_INFO("Browser created.");
    }

    // ---- CefDisplayHandler ----
    bool OnConsoleMessage(CefRefPtr<CefBrowser>, cef_log_severity_t, const CefString& message,
                          const CefString&, int) override {
        // Phase-0-validated JS->host channel. P1 logs only; P2 routes via the
        // event queues to the managed API.
        SUI_LOG_INFO("console: %s", message.ToString().c_str());
        return true;
    }

    // ---- CefLoadHandler ----
    void OnLoadingStateChange(CefRefPtr<CefBrowser> browser, bool isLoading,
                              bool, bool) override {
        SUI_LOG_INFO("load: %s", isLoading ? "loading" : "done");
    }

    void OnLoadError(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>, ErrorCode code,
                     const CefString& text, const CefString& url) override {
        SUI_LOG_ERROR("load error %d '%s' url=%s", code, text.ToString().c_str(),
                      url.ToString().c_str());
    }

private:
    CefHost* host_;
    int width_;
    int height_;
    IMPLEMENT_REFCOUNTING(SuiApp);
};

void CefHost::SendKey(bool down, UINT vk, UINT scancode, bool extended) {
    CefRefPtr<CefBrowser> b = browser_;
    if (!b) return;
    CefKeyEvent ev;
    ev.type = down ? KEYEVENT_RAWKEYDOWN : KEYEVENT_KEYUP;
    ev.windows_key_code = vk;
    ev.native_key_code = (extended ? 0x100 : 0) | (scancode << 16);
    ev.is_system_key = false;
    b->GetHost()->SendKeyEvent(ev);
}

void CefHost::SendChar(wchar_t ch) {
    CefRefPtr<CefBrowser> b = browser_;
    if (!b) return;
    CefKeyEvent ev;
    ev.type = KEYEVENT_CHAR;
    ev.windows_key_code = ch;
    ev.native_key_code = 0;
    b->GetHost()->SendKeyEvent(ev);
}

void CefHost::SendMouseMove(int x, int y) {
    CefRefPtr<CefBrowser> b = browser_;
    if (!b) return;
    CefMouseEvent ev;
    ev.x = x;
    ev.y = y;
    b->GetHost()->SendMouseMoveEvent(ev, false);
}

void CefHost::SendMouseButton(int x, int y, int button, bool down, UINT flags) {
    CefRefPtr<CefBrowser> b = browser_;
    if (!b) return;
    CefMouseEvent ev;
    ev.x = x;
    ev.y = y;
    ev.modifiers = ((flags & MK_CONTROL) ? EVENTFLAG_CONTROL_DOWN : 0) |
                   ((flags & MK_SHIFT) ? EVENTFLAG_SHIFT_DOWN : 0) |
                   ((flags & MK_LBUTTON) ? EVENTFLAG_LEFT_MOUSE_BUTTON : 0) |
                   ((flags & MK_RBUTTON) ? EVENTFLAG_RIGHT_MOUSE_BUTTON : 0);
    CefBrowserHost::MouseButtonType type = MBT_LEFT;
    if (button == 1) type = MBT_MIDDLE;
    if (button == 2) type = MBT_RIGHT;
    b->GetHost()->SendMouseClickEvent(ev, type, down, 1);
}

void CefHost::SendMouseWheel(int x, int y, int delta) {
    CefRefPtr<CefBrowser> b = browser_;
    if (!b) return;
    CefMouseEvent ev;
    ev.x = x;
    ev.y = y;
    b->GetHost()->SendMouseWheelEvent(ev, 0, delta);
}

void CefHost::SetVisible(bool visible) { visible_.store(visible); }

void CefHost::ResizeView(int width, int height) {
    viewWidth_ = width;
    viewHeight_ = height;
    CefRefPtr<CefBrowser> b = browser_;
    if (b) {
        b->GetHost()->WasResized();
        b->GetHost()->NotifyMoveOrResizeStarted();
    }
}

void CefHost::PublishFrame(const uint8_t* data, int width, int height) {
    std::lock_guard<std::mutex> lock(frameMutex_);
    front_.bgra.assign(data, data + (size_t)width * height * 4);
    front_.width = width;
    front_.height = height;
    front_.generation++;
}

bool CefHost::TakeFrame(Frame& out) {
    std::lock_guard<std::mutex> lock(frameMutex_);
    if (front_.generation <= lastSeenGeneration_ || front_.bgra.empty()) return false;
    lastSeenGeneration_ = front_.generation;
    out = front_;
    return true;
}

void CefHost::ThreadMain(const HostPaths& paths) {
    CefMainArgs mainArgs(GetModuleHandleW(nullptr));

    CefSettings settings;
    settings.no_sandbox = true;
    settings.windowless_rendering_enabled = true;
    settings.log_severity = LOGSEVERITY_WARNING;
    CefString(&settings.root_cache_path) = paths.cacheDirUtf8;
    CefString(&settings.browser_subprocess_path) = paths.workerExeUtf8;
    // Deterministic resource/locale resolution (no search-order guessing).
    CefString(&settings.resources_dir_path) = paths.resourcesDirUtf8;
    CefString(&settings.locales_dir_path) = paths.localesDirUtf8;

    CefRefPtr<SuiApp> suiApp(new SuiApp(this));
    CefRefPtr<CefApp> app = suiApp;

    if (!CefInitialize(mainArgs, settings, app.get(), nullptr)) {
        SUI_LOG_ERROR("CefInitialize failed.");
        state_ = State::Stopped;
        return;
    }
    SUI_LOG_INFO("CefInitialize OK (dedicated thread).");

    CefWindowInfo info;
    info.SetAsWindowless(kNullWindowHandle);
    info.shared_texture_enabled = false;   // CPU-side OnPaint path

    CefBrowserSettings browserSettings;
    browserSettings.background_color = CefColorSetARGB(0, 0, 0, 0);

    SUI_LOG_INFO("Loading content: %s", paths.contentFileUtf8.c_str());
    CefRefPtr<CefClient> client = suiApp;   // same instance, CefClient face
    browser_ = CefBrowserHost::CreateBrowserSync(info, client, paths.contentFileUtf8,
                                                 browserSettings, nullptr, nullptr);
    if (!browser_) {
        SUI_LOG_ERROR("CreateBrowserSync failed.");
        CefShutdown();
        state_ = State::Stopped;
        return;
    }
    state_ = State::Running;

    // Pump: CefDoMessageLoopWork on the dedicated thread (Phase-0-validated pattern).
    while (state_ == State::Running) {
        CefDoMessageLoopWork();
        // Keep the browser in sync with visibility.
        bool vis = visible_.load();
        bool wasVis = bVisible_.exchange(vis);
        if (vis != wasVis) {
            browser_->GetHost()->WasHidden(!vis);
            if (vis) browser_->GetHost()->Invalidate(PET_VIEW);
            SUI_LOG_INFO("visibility -> %s", vis ? "visible" : "hidden");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(8));
    }

    // Shutdown sequence (CEF thread).
    if (browser_) {
        browser_->GetHost()->CloseBrowser(true);
        browser_ = nullptr;
    }
    CefShutdown();
    state_ = State::Stopped;
    SUI_LOG_INFO("CEF shut down.");
}

bool CefHost::Start(const HostPaths& paths) {
    if (state_ != State::Idle) return false;
    paths_ = paths;
    thread_ = std::thread([this, paths]() { ThreadMain(paths); });
    return true;
}

void CefHost::Stop() {
    if (state_ == State::Idle || state_ == State::Stopped) return;
    state_ = State::Stopping;
    if (thread_.joinable()) thread_.join();
}

}  // namespace sui_host