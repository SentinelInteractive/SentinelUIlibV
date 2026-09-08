// SentinelUI_Worker.exe — CEF subprocess host.
// A real executable is required because CEF spawns child processes
// (renderer, utility, ...) via browser_subprocess_path. The main host module
// is a DLL (SentinelUI.asi) and cannot double as a subprocess.
//
// WIN32 subsystem on purpose: no console windows per subprocess.

#include "include/cef_app.h"

#include <windows.h>

int APIENTRY wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int) {
    CefMainArgs mainArgs(hInstance);
    int exitCode = CefExecuteProcess(mainArgs, nullptr, nullptr);
    return exitCode >= 0 ? exitCode : 0;
}