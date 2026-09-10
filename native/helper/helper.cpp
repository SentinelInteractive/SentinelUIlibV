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

#include <cwchar>
#include <string>

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

    sui_host::Log::Initialize(cfg.logDir, L"sentinelui_helper");
    SUI_LOG_INFO("SentinelUI_Host starting (content=%ls, shm=%ls, view=%dx%d, selftest=%d).",
                 cfg.contentFile.c_str(), cfg.shmName.c_str(), cfg.viewWidth, cfg.viewHeight,
                 cfg.selftest ? 1 : 0);

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
