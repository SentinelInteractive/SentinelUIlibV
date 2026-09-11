// SentinelUI_Host.exe — dedicated CEF host process.
// EXE-hosted (harness-proven): own process, dedicated CEF thread, transparent OSR,
// per-layer frames -> named shared memory for the in-game ASI compositor. Own
// process also means a page/browser crash can never take the game down.
//
// Issue #2: the boot layer loads through the sentinelui:// content scheme
// (default root "sui" -> <exe dir>\SentinelUI\content); additional layers are
// created on demand by the ASI over the command pipe.

#include <windows.h>

#include "include/cef_app.h"

#include "helper_app.h"
#include "scheme.h"
#include "suilog.h"
#include "../host/crash.h"

#include <cwchar>
#include <string>
#include <atomic>
#include <thread>

namespace {

using namespace sui_helper;

std::wstring GetArg(const std::wstring& cmdline, const std::wstring& key) {
    std::wstring pref = L"--" + key + L"=";
    auto pos = cmdline.find(pref);
    if (pos == std::wstring::npos) return L"";
    size_t start = pos + pref.size();
    size_t end;
    if (start < cmdline.size() && cmdline[start] == L'"') {
        // Quoted value: advance to the closing quote (handles spaces in paths).
        start++;
        end = cmdline.find(L'"', start);
        if (end == std::wstring::npos) end = cmdline.size();
    } else {
        end = cmdline.find(L' ', start);
        if (end == std::wstring::npos) end = cmdline.size();
    }
    return cmdline.substr(start, end - start);
}

bool HasFlag(const std::wstring& cmdline, const std::wstring& flag) {
    return cmdline.find(L"--" + flag) != std::wstring::npos;
}

std::wstring ModuleDir() {
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH * 2);
    std::wstring p(buf, n);
    auto pos = p.find_last_of(L"\\/");
    if (pos != std::wstring::npos) p = p.substr(0, pos);
    return p;
}

}  // namespace

namespace sui_helper {

namespace {
std::atomic<bool> g_parentGone{false};
}

void StartParentWatchdog(unsigned long parentPid) {
    if (!parentPid) return;
    // SYNCHRONIZE is enough to wait on the process handle; same user/session as
    // the game, so no elevation is needed.
    HANDLE parent = OpenProcess(SYNCHRONIZE, FALSE, parentPid);
    if (!parent) {
        SUI_LOG_WARN("parent watchdog: OpenProcess(%lu) failed (%lu); relying on pipe guard.",
                     parentPid, GetLastError());
        return;
    }
    std::thread([parent]() {
        WaitForSingleObject(parent, INFINITE);
        CloseHandle(parent);
        g_parentGone.store(true);
        SUI_LOG_WARN("parent process exited; fast shutdown.");
    }).detach();
    SUI_LOG_INFO("parent watchdog armed (pid %lu).", parentPid);
}

bool ParentProcessGone() { return g_parentGone.load(); }

}  // namespace sui_helper

int APIENTRY wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int) {
    // CEF subprocess dispatch (this exe doubles as renderer/utility child).
    // The app is passed in so subprocesses register the sentinelui:// standard
    // scheme too (required for consistent scheme handling across processes).
    CefMainArgs mainArgs(hInstance);
    CefRefPtr<SuiHelperApp> subprocessApp(new SuiHelperApp());
    int subprocessExit = CefExecuteProcess(mainArgs, subprocessApp.get(), nullptr);
    if (subprocessExit >= 0) return subprocessExit;

    std::wstring cmdline = GetCommandLineW();
    HelperConfig cfg;
    cfg.moduleDir = ModuleDir();
    cfg.contentFile = GetArg(cmdline, L"content");
    cfg.shmName = GetArg(cmdline, L"shm");
    if (cfg.shmName.empty()) cfg.shmName = L"Local\\SentinelUI_Frames";
    cfg.selftest = HasFlag(cmdline, L"selftest");
    cfg.timeoutSeconds = _wtoi(GetArg(cmdline, L"timeout").c_str());
    if (cfg.timeoutSeconds <= 0 && cfg.selftest) cfg.timeoutSeconds = 30;
    std::wstring size = GetArg(cmdline, L"size");
    if (!size.empty()) {
        int w = _wtoi(size.c_str());
        auto x = size.find(L'x');
        int h = x != std::wstring::npos ? _wtoi(size.substr(x + 1).c_str()) : cfg.viewHeight;
        if (w > 0) cfg.viewWidth = w;
        if (h > 0) cfg.viewHeight = h;
    }
    std::wstring paints = GetArg(cmdline, L"paints");
    if (!paints.empty()) cfg.selftestPaints = _wtoi(paints.c_str());

    cfg.cacheDir = GetArg(cmdline, L"cache");
    if (cfg.cacheDir.empty()) cfg.cacheDir = cfg.moduleDir + L"\\SentinelUI\\cache";
    cfg.logDir = GetArg(cmdline, L"logs");
    if (cfg.logDir.empty()) cfg.logDir = cfg.moduleDir + L"\\SentinelUI\\logs";
    cfg.pipeName = GetArg(cmdline, L"pipe");
    if (cfg.pipeName.empty()) cfg.pipeName = L"\\\\.\\pipe\\SentinelUI";
    cfg.parentPid = wcstoul(GetArg(cmdline, L"parent-pid").c_str(), nullptr, 10);

    sui_host::Log::Initialize(cfg.logDir, L"sentinelui_helper");
    sui_host::InstallCrashHandler(cfg.logDir, L"sentinelui_helper");
    StartParentWatchdog(cfg.parentPid);
    SUI_LOG_INFO("SentinelUI_Host starting (content=%ls, shm=%ls, view=%dx%d, selftest=%d).",
                 cfg.contentFile.c_str(), cfg.shmName.c_str(), cfg.viewWidth, cfg.viewHeight,
                 cfg.selftest ? 1 : 0);

    // Test-only: --crash-test raises a synthetic exception right after the crash
    // handler is installed so minidump capture can be verified without a real crash.
    if (HasFlag(cmdline, L"crash-test")) {
        SUI_LOG_WARN("--crash-test: raising test exception (crash handler verification).");
        RaiseException(0xE0000001u, EXCEPTION_NONCONTINUABLE, 0, nullptr);
    }

    // Default content root for the scheme: plugin "sui" -> <exe dir>\SentinelUI\content.
    Roots().SetDefaultRoot(cfg.moduleDir + L"\\SentinelUI\\content");

    // IPC: connect to the ASI's pipe, start the reader, then register handling
    // of ASI->helper commands (layer create/close, input, exec-js, viewport, ...).
    if (!cfg.selftest) {
        std::wstring err = StartIpc(cfg.pipeName);
        if (err.empty()) {
            SetupIpcCommands();
            SUI_LOG_INFO("IPC connected.");
        } else {
            SUI_LOG_WARN("IPC: %ls", err.c_str());
        }
    }

    int rc = RunBrowserProcess(cfg);
    StopIpc();
    SUI_LOG_INFO("SentinelUI_Host exiting (%d).", rc);
    return rc;
}
