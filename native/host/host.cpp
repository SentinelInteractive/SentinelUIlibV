// SentinelUI native host — module entry + exported C API.
//
// Lifecycle:
//   DllMain(PROCESS_ATTACH)  -> spawns a detached boot thread (NEVER do CEF or
//                               hook work inside DllMain loader lock).
//   boot thread              -> find game window, install swapchain+WndProc hooks,
//                               start the dedicated CEF thread.
//   SuiShutdown() (Bootstrap unload, game thread) -> stop CEF, remove hooks.

#include "exports.h"
#include "cef_host.h"
#include "hooks.h"
#include "renderer.h"
#include "suilog.h"

#include <shellapi.h>
#include <string>

namespace sui_host {
CefHost g_host;
Renderer g_renderer;
}  // namespace sui_host

namespace {

bool g_shutdownRequested = false;
sui_host::HostPaths g_paths;

std::wstring ModuleDir() {
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(GetModuleHandleW(L"SentinelUI.asi"), buf, MAX_PATH * 2);
    if (n == 0) return L"";
    std::wstring path(buf, n);
    auto pos = path.find_last_of(L"\\/");
    if (pos != std::wstring::npos) path = path.substr(0, pos);
    return path;
}

std::string ToUtf8(const std::wstring& w) {
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string out(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &out[0], n, nullptr, nullptr);
    return out;
}

struct FindCtx {
    DWORD pid;
    HWND hwnd;
};

HWND FindGameWindow() {
    FindCtx ctx = {GetCurrentProcessId(), nullptr};
    EnumWindows([](HWND hwnd, LPARAM lParam) -> BOOL {
        FindCtx* ctx = reinterpret_cast<FindCtx*>(lParam);
        DWORD hPid = 0;
        GetWindowThreadProcessId(hwnd, &hPid);
        if (hPid != ctx->pid) return TRUE;
        wchar_t cls[64] = {};
        GetClassNameW(hwnd, cls, 64);
        if (wcscmp(cls, L"grcWindow") == 0) {   // GTA V window class
            ctx->hwnd = hwnd;
            return FALSE;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&ctx));
    return ctx.hwnd;
}

DWORD WINAPI BootThread(LPVOID) {
    using namespace sui_host;
    Log::Initialize(g_paths.logDir);
    SUI_LOG_INFO("SentinelUI host loading (API v%d, module dir: %ls).",
                 SUI_API_VERSION, g_paths.moduleDir.c_str());

    // Game window may still be initializing; wait (bounded).
    HWND gameWindow = nullptr;
    for (int i = 0; i < 300 && !g_shutdownRequested; ++i) {
        gameWindow = FindGameWindow();
        if (gameWindow) break;
        Sleep(250);
    }
    if (!gameWindow) {
        SUI_LOG_ERROR("Game window not found within 75s; host aborted.");
        return 0;
    }

    if (!InstallHooks(gameWindow)) {
        SUI_LOG_ERROR("Hook installation failed; host aborted.");
        return 0;
    }

    if (!g_host.Start(g_paths)) {
        SUI_LOG_ERROR("CEF host failed to start.");
        RemoveHooks();
        return 0;
    }
    SUI_LOG_INFO("Host ready. F9 toggle visibility, F10 toggle input capture.");
    return 0;
}

void StopHost() {
    if (g_shutdownRequested) return;
    g_shutdownRequested = true;
    SUI_LOG_INFO("Shutdown requested.");
    sui_host::g_host.Stop();
    sui_host::RemoveHooks();
    SUI_LOG_INFO("Host stopped.");
}

}  // namespace

// ---- exported C API ----------------------------------------------------------

extern "C" {

int SuiGetApiVersion(void) { return SUI_API_VERSION; }

void SuiPumpEvents(void) {
    // P2: drains per-token event queues into the caller (game thread).
}

void SuiShutdown(void) {
    ::StopHost();
}

}  // extern "C"

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID) {
    if (fdwReason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hinstDLL);

        using namespace sui_host;
        g_paths.moduleDir = ModuleDir();                 // <game> root (host lives at root)
        g_paths.contentFile = g_paths.moduleDir + L"\\SentinelUI\\content\\p1_demo.html";
        g_paths.workerExe = g_paths.moduleDir + L"\\SentinelUI_Worker.exe";
        g_paths.cacheDir = g_paths.moduleDir + L"\\SentinelUI\\cache";
        g_paths.logDir = g_paths.moduleDir + L"\\SentinelUI\\logs";
        g_paths.resourcesDir = g_paths.moduleDir;
        g_paths.localesDir = g_paths.moduleDir + L"\\locales";
        g_paths.contentFileUtf8 = ToUtf8(g_paths.contentFile);
        g_paths.workerExeUtf8 = ToUtf8(g_paths.workerExe);
        g_paths.cacheDirUtf8 = ToUtf8(g_paths.cacheDir);
        g_paths.resourcesDirUtf8 = ToUtf8(g_paths.resourcesDir);
        g_paths.localesDirUtf8 = ToUtf8(g_paths.localesDir);

        HANDLE h = CreateThread(nullptr, 0, &BootThread, nullptr, 0, nullptr);
        if (h) CloseHandle(h);
    }
    return TRUE;
}