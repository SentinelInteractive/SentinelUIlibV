// SentinelUI_Host.exe — dedicated CEF host process.
// EXE-hosted (harness-proven): own process, dedicated CEF thread, transparent OSR,
// frames -> named shared memory for the in-game ASI compositor. Own process also
// means a page/browser crash can never take the game down.

#include <windows.h>

#include "include/cef_app.h"

#include "helper_app.h"
#include "shm.h"
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
    CefMainArgs mainArgs(hInstance);
    int subprocessExit = CefExecuteProcess(mainArgs, nullptr, nullptr);
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

    sui_host::Log::Initialize(cfg.logDir, L"sentinelui_helper");
    SUI_LOG_INFO("SentinelUI_Host starting (content=%ls, shm=%ls, view=%dx%d, selftest=%d).",
                 cfg.contentFile.c_str(), cfg.shmName.c_str(), cfg.viewWidth, cfg.viewHeight,
                 cfg.selftest ? 1 : 0);

    FrameShm shm;
    if (!shm.Open(cfg.shmName.c_str(), 4096, 2160)) {
        SUI_LOG_ERROR("shared memory open failed: %ls", cfg.shmName.c_str());
        return 5;
    }
    SUI_LOG_INFO("shared memory ready.");

    int rc = RunBrowserProcess(cfg, &shm);
    shm.Close();
    SUI_LOG_INFO("SentinelUI_Host exiting (%d).", rc);
    return rc;
}