// hoststub.c: minimal stand-in for the native host, built by
// tools/build-hoststub.ps1 for the managed load-path probe. It exercises the
// contract end to end without the game:
//   * a PE DLL named "SentinelUI.asi" loads via LoadLibraryW (the extension is irrelevant)
//   * exported C functions are resolved by the managed API and called across the ABI
//   * graceful shutdown via SuiShutdown.
// Exports must stay in sync with managed/SentinelUI.API/NativeHost.cs.

#include <windows.h>

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved) {
    (void)hinstDLL; (void)fdwReason; (void)lpvReserved;
    return TRUE;
}

__declspec(dllexport) int SuiGetApiVersion(void) { return 2; }
__declspec(dllexport) void SuiPumpEvents(void) {}     // no-op: the stub has no event queues
__declspec(dllexport) void SuiShutdown(void) {}       // no-op: nothing to shut down