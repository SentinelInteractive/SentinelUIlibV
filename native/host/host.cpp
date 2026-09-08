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
#include <cstdio>
#include <cstdlib>
#include <cwchar>

namespace sui_host {

CefHost g_host;
Renderer g_renderer;
bool g_shutdownRequested = false;
HostPaths g_paths;
HMODULE g_hinst = nullptr;

// ---------------------------------------------------------------------------
// Raw boot trace (Win32 file I/O only — usable even when the CRT/CEF layer is
// whatever crashed). Written to <moduleDir>\SentinelUI\logs\boot_trace.txt
// ---------------------------------------------------------------------------
namespace {

HANDLE g_traceFile = INVALID_HANDLE_VALUE;

void Trace(const char* what) {
    if (g_traceFile == INVALID_HANDLE_VALUE) {
        wchar_t mod[MAX_PATH];
        HMODULE src = g_hinst ? g_hinst : GetModuleHandleW(L"SentinelUI.asi");
        GetModuleFileNameW(src ? src : GetModuleHandleW(nullptr), mod, MAX_PATH);
        wchar_t* slash = wcsrchr(mod, L'\\');
        if (slash) *slash = 0;
        std::wstring dir = mod + std::wstring(L"\\SentinelUI\\logs");
        // Multi-level create (CreateDirectoryW is single-level only).
        {
            std::wstring cur;
            for (wchar_t ch : dir) {
                cur += ch;
                if (ch == L'\\' || ch == L'/') { if (cur.size() > 3) CreateDirectoryW(cur.c_str(), nullptr); }
            }
        }
        CreateDirectoryW(dir.c_str(), nullptr);
        std::wstring path = dir + L"\\boot_trace.txt";
        g_traceFile = CreateFileW(path.c_str(), FILE_APPEND_DATA,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                  OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    }
    if (g_traceFile != INVALID_HANDLE_VALUE && g_traceFile != nullptr) {
        char buf[512];
        int n = wsprintfA(buf, "[%lu] %s\r\n", GetCurrentThreadId(), what);
        DWORD written = 0;
        WriteFile(g_traceFile, buf, (DWORD)n, &written, nullptr);
    }
}

}  // namespace

void TraceF(const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsprintf_s(buf, _countof(buf), fmt, ap);
    va_end(ap);
    Trace(buf);
}

// ---------------------------------------------------------------------------
// Paths
// ---------------------------------------------------------------------------
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

// ---------------------------------------------------------------------------
// Game window discovery + hooks + CEF boot
// ---------------------------------------------------------------------------
struct FindCtx {
    DWORD pid;
    HWND hwnd;
};

HWND FindGameWindow() {
    FindCtx ctx = {GetCurrentProcessId(), nullptr};
    EnumWindows([](HWND hwnd, LPARAM lParam) -> BOOL {
        FindCtx* c = reinterpret_cast<FindCtx*>(lParam);
        DWORD hPid = 0;
        GetWindowThreadProcessId(hwnd, &hPid);
        if (hPid != c->pid) return TRUE;
        wchar_t cls[64] = {};
        GetClassNameW(hwnd, cls, 64);
        if (wcscmp(cls, L"grcWindow") == 0) {   // GTA V window class
            c->hwnd = hwnd;
            return FALSE;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&ctx));
    return ctx.hwnd;
}

DWORD WINAPI BootThread(LPVOID) {
    Log::Initialize(g_paths.logDir);
    TraceF("boot: log-init done");
    SUI_LOG_INFO("SentinelUI host loading (API v%d, module dir: %ls).",
                 SUI_API_VERSION, g_paths.moduleDir.c_str());
    TraceF("boot: info logged");

    // Debug escape hatch: skip the game-window wait + hooks and start CEF
    // directly, so the host can be smoke-tested outside GTA V (HostProbe).
    char dbg[2] = {0};
    if (!g_shutdownRequested &&
        GetEnvironmentVariableA("SENTINELUI_DEBUG_NOWINDOW", dbg, 2) == 1 && dbg[0] == '1') {
        SUI_LOG_INFO("DEBUG_NOWINDOW=1: skipping window wait + hooks, starting CEF directly.");
        TraceF("boot: debug path -> starting CEF");
        if (!g_host.Start(g_paths)) SUI_LOG_ERROR("CEF host failed to start (debug).");
        return 0;
    }

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
    g_host.Stop();
    RemoveHooks();
    SUI_LOG_INFO("Host stopped.");
}

}  // namespace sui_host

// ---- exported C API ----------------------------------------------------------

extern "C" {

int SuiGetApiVersion(void) { return SUI_API_VERSION; }

void SuiPumpEvents(void) {
    // P2: drains per-token event queues into the caller (game thread).
}

void SuiShutdown(void) {
    sui_host::StopHost();
}

}  // extern "C"

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID) {
    if (fdwReason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hinstDLL);
        sui_host::g_hinst = hinstDLL;
        using namespace sui_host;
        TraceF("dllmain: attach");
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
        TraceF("dllmain: boot thread created");
        if (h) CloseHandle(h);
    }
    return TRUE;
}