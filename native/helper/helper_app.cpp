#include "helper_app.h"
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