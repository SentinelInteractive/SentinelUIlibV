// SentinelUI host (ASI): module entry + exported C API.
// No CEF here: this module spawns and supervises SentinelUI_Host.exe
// (the dedicated CEF process), consumes its shared-memory frames, and hooks the
// game (Present + WndProc) to composite them.
//
// Lifecycle:
//   DllMain(PROCESS_ATTACH)  -> detached boot thread (never hook/spawn in DllMain).
//   boot thread              -> find game window -> install hooks -> start helper
//                               -> open shared memory (frame source).
//   SuiShutdown() (Bootstrap unload) -> stop helper, remove hooks.
//
// Layer stack: layer 1 is the boot page (sentinelui://sui/p1_demo.html);
// plugins add layers with SuiCreateLayer (ids >= 2). Every layer owns a frame
// mapping; the Present hook composites the visible stack in z order and the
// WndProc hook routes captured input to the focused/topmost interactive layer.
//
// Load path (critical): ScriptHookV loads this ASI from the GAME ROOT at
// process start, BEFORE RPH installs its Present hook. Bootstrap attaches to
// the already-loaded module. Loading it later (e.g. cold-load from the
// subfolder at RPH plugin-load time) makes RPH's hook guard detect a foreign
// hook on Present, revert the function and crash the D3D chain (observed
// 2026-09-11: RPH log "Hook on our Present hook detected", AV in dxgi!Present).
// Do not move this file into a subfolder unless the Present hook is replaced by
// an RPH-native render path.

#include "exports.h"
#include "crash.h"
#include "framesrc.h"
#include "hooks.h"
#include "ipc.h"
#include "layers.h"
#include "renderer.h"
#include "suilog.h"

#include <shellapi.h>
#include <atomic>
#include <string>
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <cstdarg>
#include <cwchar>

namespace sui_host {

// Paths resolved at DllMain time (module-relative; host lives at game root).
struct HostPaths {
    std::wstring moduleDir;
    std::wstring helperExe;
    std::wstring contentUrl;     // sentinelui:// URL of the boot layer
    std::wstring contentDir;     // <game>\SentinelUI\content (default "sui" root)
    std::wstring cacheDir;
    std::wstring logDir;
    std::wstring shmName;
};

Renderer g_renderer;
FrameSource g_frames;            // legacy handle for the boot-layer mapping
LayerTable g_layers;
bool g_visible = true;           // master overlay toggle (F9)
bool g_framesReady = false;
bool g_shutdownRequested = false;
bool g_duplicateInstance = false;  // second module copy in the same process (inert)
bool g_capture = false;          // input capture (F10 / SuiSetCapture)
HostPaths g_paths;
HMODULE g_hinst = nullptr;

}  // namespace sui_host

namespace {

using namespace sui_host;

// Live helper process handle (terminated on shutdown).
HANDLE g_helperProcess = nullptr;
// F11 acceptance layer (sentinelui://sui/demo_hud.html); 0 when not spawned.
std::atomic<int> g_demoLayer{0};

void Trace(const char* what);

}  // namespace

namespace sui_host {

void TraceF(const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsprintf_s(buf, _countof(buf), fmt, ap);
    va_end(ap);
    Trace(buf);
}

}  // namespace sui_host

namespace {

HANDLE g_traceFile = INVALID_HANDLE_VALUE;

void Trace(const char* what) {
    if (g_traceFile == INVALID_HANDLE_VALUE) {
        wchar_t mod[MAX_PATH];
        HMODULE src = g_hinst ? g_hinst : GetModuleHandleW(nullptr);
        GetModuleFileNameW(src, mod, MAX_PATH);
        wchar_t* slash = wcsrchr(mod, L'\\');
        if (slash) *slash = 0;
        std::wstring dir = mod + std::wstring(L"\\SentinelUI\\logs");
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

std::wstring Quote(const std::wstring& s) { return L"\"" + s + L"\""; }

void StartHelper() {
    std::wstring cmdline =
        Quote(g_paths.helperExe) +
        L" --content=" + Quote(g_paths.contentUrl) +
        L" --size=1280x720" +
        L" --shm=" + Quote(g_paths.shmName) +
        L" --logs=" + Quote(g_paths.logDir) +
        L" --cache=" + Quote(g_paths.cacheDir) +
        // Fast orphan cleanup: the helper waits on this pid and exits immediately
        // when the game process dies (kill/crash), instead of the 30s pipe guard.
        L" --parent-pid=" + std::to_wstring(GetCurrentProcessId());

    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(g_paths.helperExe.c_str(), &cmdline[0], nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        SUI_LOG_ERROR("CreateProcessW failed for helper (%lu).", GetLastError());
        return;
    }
    g_helperProcess = pi.hProcess;
    CloseHandle(pi.hThread);
    SUI_LOG_INFO("Helper process started (pid %lu).", pi.dwProcessId);

    // Wait (bounded) for the helper to create the boot layer's shared memory.
    for (int i = 0; i < 50 && !g_shutdownRequested; ++i) {
        if (g_frames.Open(g_paths.shmName.c_str(), 4096, 2160)) {
            g_framesReady = true;
            SUI_LOG_INFO("Shared-memory frame source ready.");
            break;
        }
        Sleep(100);
    }
    if (!g_framesReady) {
        SUI_LOG_ERROR("Helper shared memory not available within 5s.");
        return;
    }

    // Boot layer (id 1) takes over the mapping opened above; the layer stack
    // re-opens the same named mapping lazily (OS refcounts named mappings).
    std::string url = ToUtf8(g_paths.contentUrl);
    g_layers.CreateBase(url);
    g_frames.Close();
    SUI_LOG_INFO("Boot layer ready: %s", url.c_str());
}

void StopHelper() {
    g_framesReady = false;
    g_layers.CloseAll();
    g_demoLayer.store(0);
    g_frames.Close();
    if (g_helperProcess) {
        TerminateProcess(g_helperProcess, 0);
        WaitForSingleObject(g_helperProcess, 2000);
        CloseHandle(g_helperProcess);
        g_helperProcess = nullptr;
        SUI_LOG_INFO("Helper process stopped.");
    }
    StopIpcServer();
}

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
        if (wcscmp(cls, L"grcWindow") == 0) {
            c->hwnd = hwnd;
            return FALSE;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&ctx));
    return ctx.hwnd;
}

DWORD WINAPI BootThread(LPVOID) {
    Log::Initialize(g_paths.logDir);
    InstallCrashHandler(g_paths.logDir, L"sentinelui_host");
    TraceF("boot: log-init done");
    SUI_LOG_INFO("SentinelUI host loading (module dir: %ls).", g_paths.moduleDir.c_str());

    // Test-only: raise a synthetic exception right after the crash handler is
    // installed so minidump capture can be verified without a real crash.
    char crashDbg[2] = {0};
    if (GetEnvironmentVariableA("SENTINELUI_CRASH_TEST", crashDbg, 2) == 1 && crashDbg[0] == '1') {
        SUI_LOG_WARN("SENTINELUI_CRASH_TEST=1: raising test exception (crash handler verification).");
        RaiseException(0xE0000001u, EXCEPTION_NONCONTINUABLE, 0, nullptr);
    }

    // Debug escape hatch for out-of-game smoke tests (HostProbe): no window,
    // no hooks; start the helper and open frames only.
    char dbg[2] = {0};
    if (GetEnvironmentVariableA("SENTINELUI_DEBUG_NOWINDOW", dbg, 2) == 1 && dbg[0] == '1') {
        SUI_LOG_INFO("DEBUG_NOWINDOW=1: skipping window+hooks, starting helper only.");
        StartIpcServer();
        StartHelper();
        return 0;
    }

    HWND gameWindow = nullptr;
    for (int i = 0; i < 300 && !g_shutdownRequested; ++i) {
        gameWindow = FindGameWindow();
        if (gameWindow) break;
        Sleep(250);
    }
    if (!gameWindow) { SUI_LOG_ERROR("Game window not found within 75s; host aborted."); return 0; }

    if (!InstallHooks(gameWindow)) { SUI_LOG_ERROR("Hook installation failed."); return 0; }

    // IPC server BEFORE spawning the helper so it can connect immediately.
    StartIpcServer();
    StartHelper();
    if (!g_framesReady) { SUI_LOG_ERROR("Frames not available; overlay disabled."); }

    SUI_LOG_INFO("Host ready. F9 toggle visibility, F10 capture, F11 demo HUD layer.");
    return 0;
}

void StopHost() {
    if (g_shutdownRequested) return;
    g_shutdownRequested = true;
    SUI_LOG_INFO("Shutdown requested.");
    StopHelper();
    RemoveHooks();
    SUI_LOG_INFO("Host stopped.");
}

}  // namespace

// Input capture state (managed API + F10).
namespace sui_host {
void SetCaptureState(bool on) {
    g_capture = on;
    OnCaptureChanged(on);   // unclip/show cursor + poll mouse while captured
    SendCommand(on ? "C1\n" : "C0\n");
    SUI_LOG_INFO("Capture -> %s", on ? "on" : "off");
}
}  // namespace sui_host

// F11 demo layer (in-game acceptance for the layer stack).
namespace sui_host {
void ToggleDemoLayer() {
    // Existing layer: pure show/hide (instant, no browser create/reload).
    int id = g_demoLayer.load();
    if (id && g_layers.Exists(id)) {
        LayerTable::Info info;
        bool visible = g_layers.Get(id, info) && info.visible;
        g_layers.SetVisible(id, !visible);
        SUI_LOG_INFO("F11: demo HUD layer %d -> %s", id, visible ? "hidden" : "visible");
        return;
    }
    // First press: create the HUD strip above the boot layer, click-through.
    int created = g_layers.Create("sentinelui://sui/demo_hud.html");
    if (created) {
        g_layers.SetZ(created, 10);
        g_layers.SetClickThrough(created, true);
        g_demoLayer.store(created);
        SUI_LOG_INFO("F11: demo HUD layer %d created (z 10, click-through).", created);
    }
}
}  // namespace sui_host

// Exported C API.

extern "C" {

int SuiGetApiVersion(void) {
    // A duplicate module copy (two ASI paths loaded in one process) stays inert;
    // report it as incompatible so managed clients refuse it and use the real one.
    return sui_host::g_duplicateInstance ? -1 : SUI_API_VERSION;
}

void SuiPumpEvents(void) {
    // Drain helper->ASI events into the managed sink (game thread by contract).
    sui_host::DrainEventsToSink();
}

void SuiShutdown(void) { StopHost(); }

// Command, capture and event plumbing exports.
void SuiCmdVisible(int layerId, int visible) {
    if (layerId > 0) { sui_host::g_layers.SetVisible(layerId, visible != 0); return; }
    SendCommand(visible ? "S\n" : "H\n");   // legacy no-id form (id 1 helper-side)
}

void SuiCmdFocus(int layerId, int keyboard, int mouse) {
    if (layerId > 0) {
        sui_host::g_layers.SetFocus(layerId, keyboard != 0, mouse != 0);
        return;
    }
    SendCommand(std::string("F") + (keyboard ? "1" : "0") + (mouse ? "1" : "0") + "\n");
}

void SuiExecuteJs(int layerId, const char* jsUtf8) {
    if (!jsUtf8) return;
    std::string js(jsUtf8);
    int id = layerId > 0 ? layerId : LayerTable::kBaseLayerId;
    SendCommand("J" + std::to_string(id) + " " + std::to_string(js.size()) + ":" + js + "\n");
}

void SuiCmdNavigate(int layerId, const char* urlUtf8) {
    if (!urlUtf8) return;
    std::string url(urlUtf8);
    int id = layerId > 0 ? layerId : LayerTable::kBaseLayerId;
    SendCommand("N" + std::to_string(id) + " " + std::to_string(url.size()) + ":" + url + "\n");
}

void SuiSetCapture(int enabled) { ::sui_host::SetCaptureState(enabled != 0); }
int  SuiIsCaptureActive(void) { return g_capture ? 1 : 0; }
void SuiRegisterEventSink(void (*cb)(const char* utf8)) { ::sui_host::SetEventSink(cb); }

// Layer stack and content scheme exports.

int SuiCreateLayer(const char* urlUtf8) {
    if (!urlUtf8 || !*urlUtf8) return 0;
    return sui_host::g_layers.Create(urlUtf8);
}

void SuiCloseLayer(int layerId) {
    if (layerId <= 0) return;
    int demo = g_demoLayer.load();
    if (layerId == demo) g_demoLayer.store(0);
    sui_host::g_layers.Close(layerId);
}

void SuiSetLayerVisible(int layerId, int visible) {
    if (layerId <= 0) return;
    sui_host::g_layers.SetVisible(layerId, visible != 0);
}

int SuiIsLayerVisible(int layerId) {
    LayerTable::Info info;
    return (layerId > 0 && sui_host::g_layers.Get(layerId, info)) ? (info.visible ? 1 : 0) : 0;
}

void SuiSetLayerZ(int layerId, int z) {
    if (layerId <= 0) return;
    sui_host::g_layers.SetZ(layerId, z);
}

int SuiGetLayerZ(int layerId) {
    LayerTable::Info info;
    if (layerId <= 0 || !sui_host::g_layers.Get(layerId, info)) return 0;
    return info.z;
}

void SuiSetLayerClickThrough(int layerId, int enabled) {
    if (layerId <= 0) return;
    sui_host::g_layers.SetClickThrough(layerId, enabled != 0);
}

int SuiIsLayerClickThrough(int layerId) {
    LayerTable::Info info;
    return (layerId > 0 && sui_host::g_layers.Get(layerId, info)) ? (info.clickThrough ? 1 : 0) : 0;
}

void SuiReloadLayer(int layerId) {
    if (layerId <= 0) return;
    SendCommand("L" + std::to_string(layerId) + "\n");
}

int SuiRegisterContentRoot(const char* pluginUtf8, const char* rootPathUtf8) {
    if (!pluginUtf8 || !*pluginUtf8 || !rootPathUtf8 || !*rootPathUtf8) return 0;
    std::string body = std::string(pluginUtf8) + "|" + rootPathUtf8;
    SendCommand("R" + std::to_string(body.size()) + ":" + body + "\n");
    SUI_LOG_INFO("content root registered: sentinelui://%s/ -> %s", pluginUtf8, rootPathUtf8);
    return 1;
}

// Per-client event queue exports (host API v4).

unsigned long long SuiRegisterEventClient(void) {
    return sui_host::RegisterEventClient();
}

void SuiUnregisterEventClient(unsigned long long token) {
    sui_host::UnregisterEventClient(token);
}

void SuiPumpEventClient(unsigned long long token, void (*cb)(const char* utf8)) {
    sui_host::PumpEventClient(token, cb);
}

}  // extern "C"

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID) {
    if (fdwReason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hinstDLL);
        using namespace sui_host;
        g_hinst = hinstDLL;

        // Single-instance guard: if a second copy of this module gets loaded in
        // the same process (e.g. a stray root+subfolder deploy), it must not boot
        // a second helper or install a second hook chain. It stays inert and
        // reports API version -1 (see SuiGetApiVersion). The name is per-process
        // (pid) so an unrelated process (a probe or a second game instance)
        // does not collide with this one. The handle is kept open for the
        // process lifetime to hold the claim.
        std::wstring instanceName = L"Local\\SentinelUI_Host_Instance_v1_" +
                                    std::to_wstring(GetCurrentProcessId());
        HANDLE instanceMutex = CreateMutexW(nullptr, FALSE, instanceName.c_str());
        if (instanceMutex && GetLastError() == ERROR_ALREADY_EXISTS) {
            g_duplicateInstance = true;
            TraceF("dllmain: duplicate host instance - this module stays inert");
            return TRUE;
        }

        TraceF("dllmain: attach");
        g_paths.moduleDir = ModuleDir();
        // The module lives at the GAME ROOT (ScriptHookV must load it before RPH;
        // see the header comment), so runtime data lives under <module>\SentinelUI\.
        g_paths.helperExe = g_paths.moduleDir + L"\\SentinelUI_Host.exe";
        g_paths.contentDir = g_paths.moduleDir + L"\\SentinelUI\\content";
        // Boot layer rides the sentinelui:// scheme. file:// is gone from the
        // content path; the default "sui" root maps to content\.
        g_paths.contentUrl = L"sentinelui://sui/p1_demo.html";
        g_paths.cacheDir = g_paths.moduleDir + L"\\SentinelUI\\cache";
        g_paths.logDir = g_paths.moduleDir + L"\\SentinelUI\\logs";
        g_paths.shmName = L"Local\\SentinelUI_Frames";
        g_layers.SetShmBaseName(g_paths.shmName);

        HANDLE h = CreateThread(nullptr, 0, &BootThread, nullptr, 0, nullptr);
        TraceF("dllmain: boot thread created");
        if (h) CloseHandle(h);
    }
    return TRUE;
}
