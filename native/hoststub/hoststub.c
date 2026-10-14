// hoststub.c: minimal stand-in for the native host, built by
// tools/build-hoststub.ps1 for the managed load-path probe. It exercises the
// contract end to end without the game:
//   * a PE DLL named "SentinelUI.asi" loads via LoadLibraryW (the extension is irrelevant)
//   * exported C functions are resolved by the managed API and called across the ABI
//   * the layer model answers plausibly so facade calls run end to end
//   * graceful shutdown via SuiShutdown.
// Exports must stay in sync with managed/SentinelUI.API/NativeHost.cs (host API v4).
// Pages, events and IPC need the real helper process: the probe runs with --stub
// so those sections skip.

#include <windows.h>

static int g_nextLayerId = 2;
static int g_visible;
static int g_capture;
static unsigned long long g_nextToken = 1;

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved) {
    (void)hinstDLL; (void)fdwReason; (void)lpvReserved;
    return TRUE;
}

__declspec(dllexport) int SuiGetApiVersion(void) { return 4; }
__declspec(dllexport) void SuiPumpEvents(void) {}     // no-op: the stub has no event queues
__declspec(dllexport) void SuiShutdown(void) {}       // no-op: nothing to shut down

// Issue #1: command/capture/event plumbing.
__declspec(dllexport) void SuiCmdVisible(int layerId, int visible) { (void)layerId; g_visible = visible; }
__declspec(dllexport) void SuiCmdFocus(int layerId, int keyboard, int mouse) { (void)layerId; (void)keyboard; (void)mouse; }
__declspec(dllexport) void SuiExecuteJs(int layerId, const char* jsUtf8) { (void)layerId; (void)jsUtf8; }
__declspec(dllexport) void SuiCmdNavigate(int layerId, const char* urlUtf8) { (void)layerId; (void)urlUtf8; }
__declspec(dllexport) void SuiSetCapture(int enabled) { g_capture = enabled; }
__declspec(dllexport) int  SuiIsCaptureActive(void) { return g_capture; }
__declspec(dllexport) void SuiRegisterEventSink(void (*cb)(const char* utf8)) { (void)cb; }

// Issue #2: layer stack + content scheme. Real behavior lives in the ASI; the stub
// only needs the facade calls to succeed and the queries to answer consistently.
__declspec(dllexport) int  SuiCreateLayer(const char* urlUtf8) { (void)urlUtf8; return g_nextLayerId++; }
__declspec(dllexport) void SuiCloseLayer(int layerId) { (void)layerId; }
__declspec(dllexport) void SuiSetLayerVisible(int layerId, int visible) { (void)layerId; g_visible = visible; }
__declspec(dllexport) int  SuiIsLayerVisible(int layerId) { (void)layerId; return g_visible; }
__declspec(dllexport) void SuiSetLayerZ(int layerId, int z) { (void)layerId; (void)z; }
__declspec(dllexport) int  SuiGetLayerZ(int layerId) { (void)layerId; return 0; }
__declspec(dllexport) void SuiSetLayerClickThrough(int layerId, int enabled) { (void)layerId; (void)enabled; }
__declspec(dllexport) int  SuiIsLayerClickThrough(int layerId) { (void)layerId; return 0; }
__declspec(dllexport) void SuiReloadLayer(int layerId) { (void)layerId; }
__declspec(dllexport) int  SuiRegisterContentRoot(const char* pluginUtf8, const char* rootPathUtf8) { (void)pluginUtf8; (void)rootPathUtf8; return 0; }

// Issue #3: per-client event queues (host API v4). Tokens are handed out but never
// deliver anything; the queue behavior is the ASI's job.
__declspec(dllexport) unsigned long long SuiRegisterEventClient(void) { return g_nextToken++; }
__declspec(dllexport) void SuiUnregisterEventClient(unsigned long long token) { (void)token; }
__declspec(dllexport) void SuiPumpEventClient(unsigned long long token, void (*cb)(const char* utf8)) { (void)token; (void)cb; }
