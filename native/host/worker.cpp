// SentinelUI_Worker.exe: CEF subprocess host (legacy; not built or shipped).
// CEF spawns child processes (renderer, utility, ...) via browser_subprocess_path,
// which needs a real executable; a DLL host cannot serve as one. The helper EXE
// covers this today by spawning itself (CefExecuteProcess in native/helper/helper.cpp).
//
// WIN32 subsystem on purpose: no console windows per subprocess.

#include "include/cef_app.h"

#include <windows.h>

int APIENTRY wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int) {
    CefMainArgs mainArgs(hInstance);
    int exitCode = CefExecuteProcess(mainArgs, nullptr, nullptr);
    return exitCode >= 0 ? exitCode : 0;
}