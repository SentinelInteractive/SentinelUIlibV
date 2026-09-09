#include "helper_app.h"
#include "ipc.h"
#include "suilog.h"

#include <cstdio>
#include <cwchar>
#include <string>
#include <thread>

namespace sui_helper {

std::string ToUtf8(const std::wstring& w) {
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string out(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &out[0], n, nullptr, nullptr);
    return out;
}

static void LogLine(const std::string& msg) {
    SUI_LOG_INFO("%s", msg.c_str());
}

// ---------------------------------------------------------------------------
// Command dispatch (IPC reader thread -> app methods)
// ---------------------------------------------------------------------------
namespace {
SuiHelperApp* g_appInstance = nullptr;

bool ParseLenBody(const std::string& line, size_t pos, std::string& out) {
    size_t colon = line.find(':', pos);
    if (colon == std::string::npos) return false;
    int len = atoi(line.substr(pos, colon - pos).c_str());
    if (len < 0 || colon + 1 + (size_t)len > line.size()) return false;
    out = line.substr(colon + 1, (size_t)len);
    return true;
}

void OnIpcCommand(const std::string& line) {
    if (!g_appInstance || line.empty()) return;
    switch (line[0]) {
        case 'S': g_appInstance->SetVisible(true); break;
        case 'H': g_appInstance->SetVisible(false); break;
        case 'F': {
            bool k = line.size() > 1 && line[1] == '1';
            bool m = line.size() > 2 && line[2] == '1';
            g_appInstance->SetFocused(k, m);
            break;
        }
        case 'J': { std::string js; if (ParseLenBody(line, 1, js)) g_appInstance->QueueJs(js); break; }
        case 'N': { std::string url; if (ParseLenBody(line, 1, url)) g_appInstance->Navigate(url); break; }
        case 'V': {
            int w = 0, h = 0;
            if (sscanf_s(line.c_str() + 1, "%d %d", &w, &h) == 2) g_appInstance->SetViewport(w, h);
            break;
        }
        case 'K': {
            int d = 0, vk = 0, sc = 0, ext = 0;
            if (sscanf_s(line.c_str() + 1, "%d %d %d %d", &d, &vk, &sc, &ext) == 4)
                g_appInstance->DispatchKey(d != 0, vk, sc, ext != 0);
            break;
        }
        case 'U': { int cp = 0; if (sscanf_s(line.c_str() + 1, "%d", &cp) == 1) g_appInstance->DispatchChar(cp); break; }
        case 'M': { int x = 0, y = 0; if (sscanf_s(line.c_str() + 1, "%d %d", &x, &y) == 2) g_appInstance->DispatchMouseMove(x, y); break; }
        case 'B': {
            int b = 0, d = 0, x = 0, y = 0;
            if (sscanf_s(line.c_str() + 1, "%d %d %d %d", &b, &d, &x, &y) == 4)
                g_appInstance->DispatchMouseButton(b, d != 0, x, y);
            break;
        }
        case 'W': {
            int dd = 0, x = 0, y = 0;
            if (sscanf_s(line.c_str() + 1, "%d %d %d", &dd, &x, &y) == 3)
                g_appInstance->DispatchMouseWheel(dd, x, y);
            break;
        }
        case 'Q': StopIpc(); break;
        default: break;
    }
}
}  // namespace

void SetupIpcCommands() { SetCommandHandler(&OnIpcCommand); }

// ---------------------------------------------------------------------------
// App command methods (called from the IPC reader thread)
// ---------------------------------------------------------------------------
void SuiHelperApp::SetVisible(bool visible) {
    if (!browser_) return;
    browser_->GetHost()->WasHidden(!visible);
    if (visible) browser_->GetHost()->Invalidate(PET_VIEW);
}

void SuiHelperApp::SetFocused(bool keyboard, bool mouse) {
    if (!browser_) return;
    browser_->GetHost()->SetFocus(keyboard);
}

void SuiHelperApp::SetViewport(int width, int height) {
    if (width <= 0 || height <= 0) return;
    viewWidth_ = width;
    viewHeight_ = height;
    SUI_LOG_INFO("helper viewport -> %dx%d", width, height);
    if (browser_) {
        browser_->GetHost()->WasResized();
        browser_->GetHost()->NotifyMoveOrResizeStarted();
    }
}

void SuiHelperApp::Navigate(const std::string& urlUtf8) {
    if (!browser_) return;
    browser_->GetMainFrame()->LoadURL(urlUtf8);
}

void SuiHelperApp::ExecuteJs(const std::string& javascript) {
    if (!browser_) return;
    browser_->GetMainFrame()->ExecuteJavaScript(javascript, "sentinelui://", 0);
}

// Software cursor: GTA V hides the OS cursor and OSR draws none — the page renders
// its own click-through arrow so users can see where they're pointing.
const char* kCursorJs =
    "(function(){"
    "if(document.getElementById('sui-cursor'))return;"
    "var c=document.createElement('div');"
    "c.id='sui-cursor';"
    "c.style.cssText='position:fixed;left:0;top:0;width:20px;height:20px;"
    "pointer-events:none;z-index:2147483647;mix-blend-mode:difference;';"
    "c.innerHTML='<svg width=\"20\" height=\"20\" viewBox=\"0 0 20 20\">"
    "<path d=\"M2 2 L10 14 L12 12 L16 18 L18 16 L14 10 L16 8 Z\" "
    "fill=\"rgba(255,255,255,.95)\" stroke=\"rgba(0,0,0,.85)\" stroke-width=\"1.5\"/></svg>';"
    "document.body.appendChild(c);"
    "var move=function(e){c.style.left=e.clientX+'px';c.style.top=e.clientY+'px';};"
    "document.addEventListener('mousemove',move,true);"
    "document.addEventListener('mousedown',function(){c.style.transform='translate(-2px,-1px) scale(.9)';},true);"
    "document.addEventListener('mouseup',function(){c.style.transform='translate(-2px,-2px)';},true);"
    "})();";

void SuiHelperApp::QueueJs(const std::string& javascript) {
    std::lock_guard<std::mutex> lock(jsMutex_);
    jsQueue_.push_back(javascript);
}

void SuiHelperApp::FlushJsQueue() {
    if (!browser_) return;
    std::vector<std::string> batch;
    {
        std::lock_guard<std::mutex> lock(jsMutex_);
        if (jsQueue_.empty()) return;
        batch.swap(jsQueue_);
    }
    for (const auto& js : batch) ExecuteJs(js);
}

void SuiHelperApp::DispatchKey(bool down, int vk, int scancode, bool extended) {
    if (!browser_) return;
    CefKeyEvent ev;
    ev.type = down ? KEYEVENT_RAWKEYDOWN : KEYEVENT_KEYUP;
    ev.windows_key_code = vk;
    ev.native_key_code = (extended ? 0x100 : 0) | (scancode << 16);
    ev.is_system_key = false;
    browser_->GetHost()->SendKeyEvent(ev);
}

void SuiHelperApp::DispatchChar(int codePoint) {
    if (!browser_) return;
    CefKeyEvent ev;
    ev.type = KEYEVENT_CHAR;
    ev.windows_key_code = codePoint;
    ev.native_key_code = 0;
    browser_->GetHost()->SendKeyEvent(ev);
}

void SuiHelperApp::DispatchMouseMove(int x, int y) {
    if (!browser_) return;
    CefMouseEvent ev;
    ev.x = x;
    ev.y = y;
    browser_->GetHost()->SendMouseMoveEvent(ev, false);
}

void SuiHelperApp::DispatchMouseButton(int button, bool down, int x, int y) {
    if (!browser_) return;
    CefMouseEvent ev;
    ev.x = x;
    ev.y = y;
    CefBrowserHost::MouseButtonType type = MBT_LEFT;
    if (button == 1) type = MBT_MIDDLE;
    if (button == 2) type = MBT_RIGHT;
    browser_->GetHost()->SendMouseClickEvent(ev, type, down, 1);
}

void SuiHelperApp::DispatchMouseWheel(int delta, int x, int y) {
    if (!browser_) return;
    CefMouseEvent ev;
    ev.x = x;
    ev.y = y;
    browser_->GetHost()->SendMouseWheelEvent(ev, 0, delta);
}

// ---------------------------------------------------------------------------
// CEF callbacks -> pipe events (console/load) + local log
// ---------------------------------------------------------------------------
int RunBrowserProcess(const HelperConfig& cfg, FrameShm* shm) {
    std::wstring pageUrl = L"file:///" + cfg.contentFile;
    std::string::size_type p = 0;
    while ((p = pageUrl.find(L'\\', p)) != std::wstring::npos) pageUrl.replace(p, 1, L"/");
    std::string pageUrlUtf8 = ToUtf8(pageUrl);

    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::wstring exePathW(exePath);

    CefMainArgs mainArgs(GetModuleHandleW(nullptr));
    CefSettings settings;
    settings.no_sandbox = true;
    settings.windowless_rendering_enabled = true;
    settings.log_severity = LOGSEVERITY_WARNING;
    CefString(&settings.root_cache_path) = ToUtf8(cfg.cacheDir);
    CefString(&settings.browser_subprocess_path) = ToUtf8(exePathW);   // self-spawn (EXE)

    CefRefPtr<SuiHelperApp> app(new SuiHelperApp(shm));
    app->SetProcessor(&LogLine);
    g_appInstance = app.get();

    if (!CefInitialize(mainArgs, settings, app.get(), nullptr)) {
        SUI_LOG_ERROR("CefInitialize failed.");
        return 1;
    }
    SUI_LOG_INFO("CefInitialize OK w/ view %dx%d; loading %s", cfg.viewWidth, cfg.viewHeight,
                 pageUrlUtf8.c_str());

    CefWindowInfo info;
    info.SetAsWindowless(kNullWindowHandle);

    CefBrowserSettings browserSettings;
    browserSettings.background_color = CefColorSetARGB(0, 0, 0, 0);

    CefRefPtr<CefBrowser> browser =
        CefBrowserHost::CreateBrowserSync(info, app.get(), pageUrlUtf8, browserSettings, nullptr, nullptr);
    if (!browser) {
        SUI_LOG_ERROR("CreateBrowserSync failed.");
        CefShutdown();
        return 2;
    }

    // Pump loop (dedicated-thread CefDoMessageLoopWork, harness-proven).
    unsigned __int64 endTick = cfg.timeoutSeconds ? GetTickCount64() + (unsigned __int64)cfg.timeoutSeconds * 1000 : 0;
    bool quitting = false;
    while (!quitting) {
        CefDoMessageLoopWork();
        app->FlushJsQueue();
        if (cfg.timeoutSeconds && GetTickCount64() >= endTick) quitting = true;
        if (cfg.selftest && app->paintCount >= cfg.selftestPaints) quitting = true;
        std::this_thread::sleep_for(std::chrono::milliseconds(8));
    }

    // Self-test: verify the shared-memory frame actually contains rendered content.
    if (cfg.selftest) {
        const FrameHeader* h = shm->Header();
        const uint8_t* px = shm->Pixels();
        if (!h || h->state == 0 || h->generation == 0 || !px) {
            SUI_LOG_ERROR("SELFTEST FAIL: no frame published.");
            browser->GetHost()->CloseBrowser(true);
            CefShutdown();
            return 3;
        }
        auto alphaAt = [&](int x, int y) {
            size_t off = (size_t)y * h->width * 4 + (size_t)x * 4;
            return (int)px[off + 3];
        };
        int aBg = alphaAt(10, 10);
        int aTile = alphaAt(80, 60);
        SUI_LOG_INFO("SELFTEST: gen=%llu %ux%u alpha(10,10)=%d alpha(80,60)=%d",
                     (unsigned long long)h->generation, h->width, h->height, aBg, aTile);
        bool ok = (h->generation > 0) && ((int)h->width == cfg.viewWidth) && (aBg < 40) && (aTile > 100);
        SUI_LOG_INFO(ok ? "SELFTEST PASS" : "SELFTEST FAIL");
        browser->GetHost()->CloseBrowser(true);
        CefShutdown();
        return ok ? 0 : 4;
    }

    SUI_LOG_INFO("Helper running (timeout=%d).", cfg.timeoutSeconds);
    browser->GetHost()->CloseBrowser(true);
    CefShutdown();
    return 0;
}

}  // namespace sui_helper