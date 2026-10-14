// SentinelUI native host: exported C API contract.
// Must stay in sync with managed/SentinelUI.API/NativeHost.cs.
//
// Version history (SUI_API_VERSION):
//   v3: multi-layer stack + sentinelui:// content scheme, additive to the v2
//       layer/command exports. Layer ids: 1 = boot layer; ids >= 2 are
//       allocated by SuiCreateLayer.
//   v4: cross-AppDomain event clients, additive to v3. Old API assemblies keep
//       working through SuiRegisterEventSink/SuiPumpEvents.

#pragma once

#include <stdint.h>

#define SUI_API_VERSION 4

#ifdef __cplusplus
extern "C" {
#endif

// Returns the export-table API version. First call any plugin makes.
__declspec(dllexport) int SuiGetApiVersion(void);

// Drains host event queues. MUST be called on the game thread (per-frame).
__declspec(dllexport) void SuiPumpEvents(void);

// Graceful host shutdown. Only called from the RPH Bootstrap unload path.
__declspec(dllexport) void SuiShutdown(void);

// Command, capture and event plumbing exports.
__declspec(dllexport) void SuiCmdVisible(int layerId, int visible);
__declspec(dllexport) void SuiCmdFocus(int layerId, int keyboard, int mouse);
__declspec(dllexport) void SuiExecuteJs(int layerId, const char* jsUtf8);
__declspec(dllexport) void SuiCmdNavigate(int layerId, const char* urlUtf8);
__declspec(dllexport) void SuiSetCapture(int enabled);
__declspec(dllexport) int  SuiIsCaptureActive(void);
__declspec(dllexport) void SuiRegisterEventSink(void (*cb)(const char* utf8));

// Layer stack and content scheme exports.
// Creates a layer loading urlUtf8 (sentinelui://<plugin>/<path>, or https:// when
// the plugin opted into remote content). Returns the native layer id (0 on
// failure: budget exhausted / host not ready).
__declspec(dllexport) int  SuiCreateLayer(const char* urlUtf8);

// Closes a layer; the id may be reused after the 999-id wrap.
__declspec(dllexport) void SuiCloseLayer(int layerId);

// Visibility / stacking / input policy per layer.
__declspec(dllexport) void SuiSetLayerVisible(int layerId, int visible);
__declspec(dllexport) int  SuiIsLayerVisible(int layerId);
__declspec(dllexport) void SuiSetLayerZ(int layerId, int z);
__declspec(dllexport) int  SuiGetLayerZ(int layerId);
__declspec(dllexport) void SuiSetLayerClickThrough(int layerId, int enabled);
__declspec(dllexport) int  SuiIsLayerClickThrough(int layerId);
__declspec(dllexport) void SuiReloadLayer(int layerId);

// Registers a local content root for the sentinelui:// scheme:
//   sentinelui://<plugin>/<relpath> -> <rootPathUtf8>\<relpath>
// The helper resolves every request against the root; directory traversal
// outside it is refused. Returns 1 on success.
__declspec(dllexport) int  SuiRegisterContentRoot(const char* pluginUtf8,
                                                  const char* rootPathUtf8);

// Event flow: helper -> pipe -> queue -> SuiPumpEvents -> sink.
// Sink line: "<nativeLayerId>|<kind>:<body>" (e.g. "1|console:NUI_...").

// Per-client event queue exports (host API v4).
// Cross-AppDomain delivery: each domain registers a client (token), and pumps
// only its own queue on its own thread. Every inbound event is copied to every
// client queue by whichever client (or legacy sink) pumps first. Queues are
// bounded (oldest events dropped + logged). Register returns 0 on failure.
__declspec(dllexport) unsigned long long SuiRegisterEventClient(void);
__declspec(dllexport) void SuiUnregisterEventClient(unsigned long long token);
// Drain the calling client's queue through cb (called on the caller's thread).
__declspec(dllexport) void SuiPumpEventClient(unsigned long long token,
                                              void (*cb)(const char* utf8));

#ifdef __cplusplus
}
#endif
