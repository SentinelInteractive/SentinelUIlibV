// SentinelUI Phase 0 — CEF OSR validation harness.
//
// Answers the gate questions that decide the CEF version pin for the in-game host:
//   Q1  Can CefInitialize run on a DEDICATED THREAD (not the process main thread) and keep
//       working? The in-game host must init CEF on its own thread because the GTA V main thread
//       cannot pump CEF.
//   Q2  Does transparent OSR OnPaint fire reliably (post-M128/M130 churn) with --disable-gpu?
//   Q3  Does CefDoMessageLoopWork (backed by OnScheduleMessagePumpWork semantics) drive the
//       message loop, or do we need CefRunMessageLoop?
//   Q4  Does the JS->host console channel work (console.log("NUI_...") observed in the C++
//       OnConsoleMessage handler)?
//   Q5  Can we upload an OnPaint BGRA buffer to a D3D11 texture and read it back (the host's
//       texture pipeline, minus the swapchain hook)?
//
// The harness doubles as its own browser subprocess (standard CefExecuteProcess pattern), so no
// separate worker exe is needed at this stage.
//
// Build: see CMakeLists.txt in this folder (uses the pinned CEF distribution rules).

#include "include/cef_app.h"
#include "include/cef_browser.h"
#include "include/cef_client.h"
#include "include/cef_render_handler.h"
#include "include/cef_command_line.h"
#include "include/cef_version.h"

#include <windows.h>
#include <d3d11.h>

#include <cstdarg>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "d3d11.lib")

namespace {

constexpr int kWidth = 1280;
constexpr int kHeight = 720;
constexpr int kPaintsForStability = 20;

std::vector<uint8_t> g_lastBuffer;         // BGRA, kWidth*kHeight*4
volatile long g_paintCount = 0;
volatile long g_consoleMessageSeen = 0;
volatile bool g_pageLoaded = false;
std::chrono::steady_clock::time_point g_tInit;
std::chrono::steady_clock::time_point g_tFirstPaint;

void Log(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    std::printf("[OSR] "); std::vprintf(fmt, ap); std::printf("\n");
    va_end(ap);
}

// Minimal crash-locator: a plain file we can read even when the console output is lost
// (failfast crashes die before stdout flushes).
static FILE* g_traceFile = nullptr;
void TraceLog(const char* fmt, ...) {
    if (!g_traceFile) g_traceFile = std::fopen("harness_trace.txt", "a");
    if (!g_traceFile) return;
    va_list ap; va_start(ap, fmt);
    std::vfprintf(g_traceFile, fmt, ap);
    std::fprintf(g_traceFile, "\n");
    std::fflush(g_traceFile);
    va_end(ap);
}

// ---------------------------------------------------------------------------
// Harness client: implements CefClient, CefRenderHandler, CefLifeSpanHandler,
// CefDisplayHandler and the command-line switch sink (CefApp).
// ---------------------------------------------------------------------------
class HarnessApp : public CefApp,
                   public CefClient,
                   public CefRenderHandler,
                   public CefLifeSpanHandler,
                   public CefDisplayHandler,
                   public CefLoadHandler {
public:
    // CefApp
    void OnBeforeCommandLineProcessing(const CefString& /*process_type*/,
                                       CefRefPtr<CefCommandLine> command_line) override {
        // Force the software OSR path — same posture as the in-game host (critique R2/D3):
        // GPU-process rendering is not production-ready for partial updates in OSR.
        command_line->AppendSwitch("disable-gpu");
        command_line->AppendSwitch("disable-gpu-compositing");
        // Avoid noise from remote process spawns during the harness run.
        command_line->AppendSwitchWithValue("disable-features", "SpareRendererProcessForSitePerProcess");
    }

    // CefClient
    CefRefPtr<CefRenderHandler> GetRenderHandler() override { return this; }
    CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override { return this; }
    CefRefPtr<CefDisplayHandler> GetDisplayHandler() override { return this; }
    CefRefPtr<CefLoadHandler> GetLoadHandler() override { return this; }

    // CefLoadHandler
    void OnLoadingStateChange(CefRefPtr<CefBrowser> browser,
                              bool isLoading,
                              bool canGoBack,
                              bool canGoForward) override {
        Log("OnLoadingStateChange: %s (canGoBack=%d canGoForward=%d)",
            isLoading ? "loading" : "done", canGoBack, canGoForward);
    }

    void OnLoadError(CefRefPtr<CefBrowser> browser,
                     CefRefPtr<CefFrame> frame,
                     ErrorCode errorCode,
                     const CefString& errorText,
                     const CefString& failedUrl) override {
        Log("OnLoadError: %d '%s' url=%s", errorCode, errorText.ToString().c_str(), failedUrl.ToString().c_str());
    }

    // CefRenderHandler
    void GetViewRect(CefRefPtr<CefBrowser> /*browser*/, CefRect& rect) override {
        rect = CefRect(0, 0, kWidth, kHeight);
    }

    bool GetScreenInfo(CefRefPtr<CefBrowser> /*browser*/, CefScreenInfo& screen) override {
        screen.device_scale_factor = 1.0f;
        screen.rect = CefRect(0, 0, kWidth, kHeight);
        screen.available_rect = screen.rect;
        return true;
    }

    void OnPaint(CefRefPtr<CefBrowser> browser,
                 PaintElementType type,
                 const RectList& /*dirtyRects*/,
                 const void* buffer,
                 int width,
                 int height) override {
        if (type != PET_VIEW) return;
        const size_t byteCount = static_cast<size_t>(width) * height * 4u;
        if (byteCount != g_lastBuffer.size()) g_lastBuffer.resize(byteCount);

        int count = InterlockedIncrement(&g_paintCount);
        if (count == 1) g_tFirstPaint = std::chrono::steady_clock::now();
        if (count <= 8) {
            // Paint cadence insight: how often does Chromium push frames for a static page?
            static long last = 0;
            auto now = std::chrono::steady_clock::now();
            static auto tLast = now;
            Log("paint #%d (%dx%d, +%lld ms since previous)",
                count, width, height,
                std::chrono::duration_cast<std::chrono::milliseconds>(now - tLast).count());
            tLast = now;
        }

        std::memcpy(g_lastBuffer.data(), buffer, byteCount);
    }

    // CefLifeSpanHandler
    void OnAfterCreated(CefRefPtr<CefBrowser> browser) override {
        if (browser->IsPopup()) return;
        g_tInit = std::chrono::steady_clock::now();
        Log("Browser created (pid %d).", GetCurrentProcessId());
    }

    bool DoClose(CefRefPtr<CefBrowser> /*browser*/) override { return false; }

    // CefDisplayHandler
    bool OnConsoleMessage(CefRefPtr<CefBrowser> /*browser*/,
                          cef_log_severity_t /*level*/,
                          const CefString& message,
                          const CefString& /*source*/,
                          int /*line*/) override {
        std::string msg = message.ToString();
        Log("console: %s", msg.c_str());
        if (msg.rfind("NUI_TEST:", 0) == 0) {
            InterlockedIncrement(&g_consoleMessageSeen);
            Log("Q4 PASS: JS->host console channel works ('%s').", msg.c_str());
        }
        return true;
    }

private:
    IMPLEMENT_REFCOUNTING(HarnessApp);
};

// ---------------------------------------------------------------------------
// D3D11 texture upload/readback roundtrip (Q5).
// ---------------------------------------------------------------------------
bool D3d11Roundtrip(std::vector<uint8_t>& buffer) {
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    D3D_FEATURE_LEVEL featureLevel;
    D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};

    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                   D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 2,
                                   D3D11_SDK_VERSION, &device, &featureLevel, &context);
    if (FAILED(hr)) {
        Log("Q5 hardware device failed (0x%08X); falling back to WARP...", hr);
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
                               D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 2,
                               D3D11_SDK_VERSION, &device, &featureLevel, &context);
    }
    if (FAILED(hr)) { Log("Q5 FAIL: D3D11CreateDevice 0x%08X", hr); return false; }

    // Texture
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = kWidth; desc.Height = kHeight;
    desc.MipLevels = 1; desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    ID3D11Texture2D* tex = nullptr;
    hr = device->CreateTexture2D(&desc, nullptr, &tex);
    if (FAILED(hr)) { Log("Q5 FAIL: CreateTexture2D 0x%08X", hr); return false; }

    context->UpdateSubresource(tex, 0, nullptr, buffer.data(), kWidth * 4u, 0);

    // Staging + readback
    D3D11_TEXTURE2D_DESC stageDesc = desc;
    stageDesc.Usage = D3D11_USAGE_STAGING;
    stageDesc.BindFlags = 0;
    stageDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D* stage = nullptr;
    hr = device->CreateTexture2D(&stageDesc, nullptr, &stage);
    if (FAILED(hr)) { Log("Q5 FAIL: CreateTexture2D(staging) 0x%08X", hr); return false; }

    context->CopyResource(stage, tex);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    hr = context->Map(stage, 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) { Log("Q5 FAIL: Map 0x%08X", hr); return false; }

    const uint8_t* src = buffer.data();
    const uint8_t* dst = static_cast<const uint8_t*>(mapped.pData);

    // Compare a grid of pixels (skip row padding).
    const int step = 37;
    int mismatches = 0;
    int compared = 0;
    for (int y = 0; y < kHeight; y += step) {
        for (int x = 0; x < kWidth; x += step) {
            size_t sOff = (size_t)y * kWidth * 4 + (size_t)x * 4;
            size_t dOff = (size_t)y * (size_t)mapped.RowPitch + (size_t)x * 4;
            if (std::memcmp(src + sOff, dst + dOff, 4) != 0) {
                if (mismatches < 5)
                    Log("   mismatch @(%d,%d) src=%02X%02X%02X%02X dst=%02X%02X%02X%02X", x, y,
                        src[sOff], src[sOff+1], src[sOff+2], src[sOff+3],
                        dst[dOff], dst[dOff+1], dst[dOff+2], dst[dOff+3]);
                mismatches++;
            }
            compared++;
        }
    }

    context->Unmap(stage, 0);

    // Report the alpha story at two known points on the page:
    // (10,10) = page background (transparent), (80,60) = colored tile (opaque-ish).
    auto alphaAt = [&](int x, int y) {
        size_t off = (size_t)y * kWidth * 4 + (size_t)x * 4;
        return static_cast<int>(src[off + 3]);
    };
    Log("alpha(10,10)=%d (expect ~0 transparent bg), alpha(80,60)=%d (expect >100 tile)", alphaAt(10, 10), alphaAt(80, 60));

    stage->Release(); tex->Release();
    context->Release(); device->Release();

    bool pass = (mismatches == 0);
    Log("Q5 %s: D3D11 upload+readback roundtrip, %d pixels compared, %d mismatches.",
        pass ? "PASS" : "FAIL", compared, mismatches);
    return pass;
}

// ---------------------------------------------------------------------------
// Browser-process flow: runs on the dedicated CEF thread (Q1).
// ---------------------------------------------------------------------------
int RunBrowserProcess(const CefMainArgs& args) {
    CefSettings settings;
    settings.no_sandbox = true;
    settings.windowless_rendering_enabled = true;
    settings.log_severity = LOGSEVERITY_INFO;
    // Absolute cache path (CEF rejects relative roots).
    char exeDir[MAX_PATH]{};
    GetModuleFileNameA(nullptr, exeDir, MAX_PATH);
    std::string cachePath(exeDir);
    auto pos = cachePath.find_last_of('\\');
    if (pos != std::string::npos) cachePath = cachePath.substr(0, pos) + "\\harness_cef_cache";
    CefString(&settings.root_cache_path) = cachePath;
    // Own subprocess: this exe doubles as the renderer/GPU-less child.
    wchar_t wexePath[MAX_PATH]; GetModuleFileNameW(nullptr, wexePath, MAX_PATH);
    CefString(&settings.browser_subprocess_path) = wexePath;

    CefRefPtr<HarnessApp> app(new HarnessApp());

    auto t0 = std::chrono::steady_clock::now();
    if (!CefInitialize(args, settings, app.get(), nullptr)) {
        Log("Q1 FAIL: CefInitialize returned false.");
        return 1;
    }
    auto t1 = std::chrono::steady_clock::now();
    Log("Q1 CefInitialize OK on dedicated thread (%lld ms).",
        std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count());

    CefWindowInfo info;
    // OSR is always transparent since CEF ~104 (background_color in browser settings
    // controls the composite alpha). No HWND — pure offscreen like the in-game host.
    info.SetAsWindowless(kNullWindowHandle);

    // Load from a real local file (file://) — content delivery closest to the final
    // sentinelui:// scheme, and immune to data-URL encoding issues.
    const char* kPageHtml =
        "<!DOCTYPE html><html><body style=\"margin:0;background:transparent\">"
        "<div style=\"position:absolute;left:16px;top:16px;width:260px;height:120px;"
        "background:rgba(0,120,255,0.65);padding:12px;color:#fff;font:bold 28px sans-serif\">"
        "SentinelUI harness</div>"
        "<div style=\"position:absolute;left:16px;top:160px;color:#222;font:14px monospace\">"
        "transparent OSR page - background should show through</div>"
        "<script>setTimeout(function(){ console.log('NUI_TEST:hello-from-page'); },300);</script>"
        "</body></html>";

    char pageExeDir[MAX_PATH]{};
    GetModuleFileNameA(nullptr, pageExeDir, MAX_PATH);
    std::string exeDirPath(pageExeDir);
    auto slash = exeDirPath.find_last_of('\\');
    if (slash != std::string::npos) exeDirPath = exeDirPath.substr(0, slash);
    const std::string pagePath = exeDirPath + "\\harness_page.html";
    {
        std::ofstream page(pagePath, std::ios::binary);
        page.write(kPageHtml, static_cast<std::streamsize>(std::strlen(kPageHtml)));
    }
    std::string pageUrl = "file:///" + pagePath;
    std::string::size_type p = 0;
    while ((p = pageUrl.find('\\', p)) != std::string::npos) pageUrl.replace(p, 1, "/");

    CefBrowserSettings browserSettings;
    browserSettings.background_color = CefColorSetARGB(0, 0, 0, 0);

    CefRefPtr<CefBrowser> browser =
        CefBrowserHost::CreateBrowserSync(info, app.get(), pageUrl, browserSettings, nullptr, nullptr);
    if (!browser) {
        Log("Q2 FAIL: CreateBrowserSync returned null.");
        return 1;
    }

    // Message pump: dedicated-thread CefDoMessageLoopWork (Q3) — the pattern the in-game host uses.
    // NOTE: a static page paints a couple of times after load, then goes fully idle
    // (observed cadence: paints #1..n shortly after load, then nothing) — this is exactly
    // the "no redraw when nothing changed" behavior the in-game host relies on.
    auto pumpStart = std::chrono::steady_clock::now();
    while (true) {
        CefDoMessageLoopWork();
        bool enough = (InterlockedAdd(&g_paintCount, 0) >= 2) && g_consoleMessageSeen;
        if (enough) break;
        if (std::chrono::steady_clock::now() - pumpStart > std::chrono::seconds(10))
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(8));
    }

    auto tEnd = std::chrono::steady_clock::now();
    long paints = InterlockedAdd(&g_paintCount, 0);
    Log("Paints received: %ld in %lld ms (first paint %lld ms after init).",
        paints,
        std::chrono::duration_cast<std::chrono::milliseconds>(tEnd - pumpStart).count(),
        g_tFirstPaint.time_since_epoch().count() ? std::chrono::duration_cast<std::chrono::milliseconds>(g_tFirstPaint - g_tInit).count() : -1);

    bool ok = (paints >= 2) && g_consoleMessageSeen != 0;

    // Q4 result
    if (g_consoleMessageSeen) Log("Q4 PASS (see console line above).");
    else { Log("Q4 FAIL: no NUI_TEST console message observed."); ok = false; }

    // Q5
    if (ok) ok = D3d11Roundtrip(g_lastBuffer);

    // Save the last frame for visual inspection.
    if (!g_lastBuffer.empty()) {
        std::ofstream ppm("harness_frame.ppm", std::ios::binary);
        ppm << "P6\n" << kWidth << " " << kHeight << "\n255\n";
        std::vector<uint8_t> rgb(kWidth * kHeight * 3);
        for (int y = 0; y < kHeight; ++y)
            for (int x = 0; x < kWidth; ++x) {
                size_t b = (size_t)y * kWidth * 4 + (size_t)x * 4;
                size_t r = ((size_t)y * kWidth + (size_t)x) * 3;
                rgb[r] = g_lastBuffer[b + 2];
                rgb[r + 1] = g_lastBuffer[b + 1];
                rgb[r + 2] = g_lastBuffer[b];
            }
        ppm.write(reinterpret_cast<const char*>(rgb.data()), static_cast<std::streamsize>(rgb.size()));
        Log("Saved harness_frame.ppm (%d x %d) for visual inspection.", kWidth, kHeight);
    }

    browser->GetHost()->CloseBrowser(true);
    CefShutdown();

    Log(ok ? "HARNESS RESULT: PASS" : "HARNESS RESULT: FAIL");
    return ok ? 0 : 2;
}

}  // namespace

int wmain(int argc, wchar_t* argv[]) {
    TraceLog("wmain start");

    CefMainArgs mainArgs(GetModuleHandleW(nullptr));
    int subprocessExit = CefExecuteProcess(mainArgs, nullptr, nullptr);
    TraceLog("CefExecuteProcess returned %d", subprocessExit);
    if (subprocessExit >= 0) return subprocessExit;   // we were spawned as a child process

    std::thread cefThread([]() {
        TraceLog("cef thread start");
        int result = RunBrowserProcess(CefMainArgs(GetModuleHandleW(nullptr)));
        TraceLog("RunBrowserProcess returned %d", result);
        std::exit(result);
    });
    cefThread.detach();

    // Keep the launcher alive until the CEF thread decides. Simple event-based wait:
    // (the CEF thread calls std::exit, which ends the process cleanly.)
    for (;;) std::this_thread::sleep_for(std::chrono::hours(1));
    return 0;
}