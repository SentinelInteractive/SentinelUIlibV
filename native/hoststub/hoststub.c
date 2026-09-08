// hoststub.c — minimal stand-in for the future native CEF host.
// Proves the P0 load-path/marshalling contract end to end WITHOUT the game:
//   * a PE DLL named "SentinelUI.asi" loads fine via LoadLibraryW (extension is irrelevant)
//   * exported C functions are resolved by the managed API and called across the ABI
//   * graceful shutdown via SuiShutdown.
// Exports must stay in sync with managed/SentinelUI.API/NativeHost.cs.

#include <windows.h>

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved) {
    (void)hinstDLL; (void)fdwReason; (void)lpvReserved;
    return TRUE;
}

__declspec(dllexport) int SuiGetApiVersion(void) { return 2; }
__declspec(dllexport) void SuiPumpEvents(void) {}     // no-op; queues land in P2
__declspec(dllexport) void SuiShutdown(void) {}       // no-op for the stub