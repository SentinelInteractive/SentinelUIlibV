// SentinelUI native host — exported C API contract (v3).
// MUST stay in sync with managed/SentinelUI.API/NativeHost.cs.
//
// v3 = issue #2 (multi-layer stack + sentinelui:// content scheme), additive
//      to the v2 layer/command exports. Layer ids: 1 = boot layer; ids >= 2
//      are allocated by SuiCreateLayer.

#pragma once
#ifndef SENTINEL_HOST_EXPORTS_H
#define SENTINEL_HOST_EXPORTS_H

#include <stdint.h>

#define SUI_API_VERSION 3

#ifdef __cplusplus
extern "C" {
#endif

// Returns the export-table API version. First call any plugin makes.
__declspec(dllexport) int SuiGetApiVersion(void);

// Drains host event queues. MUST be called on the game thread (per-frame).
__declspec(dllexport) void SuiPumpEvents(void);

// Graceful host shutdown. Only called from the RPH Bootstrap unload path.
__declspec(dllexport) void SuiShutdown(void);

// ---- issue #1 exports (command/capture/event plumbing) ----
__declspec(dllexport) void SuiCmdVisible(int layerId, int visible);
__declspec(dllexport) void SuiCmdFocus(int layerId, int keyboard, int mouse);
__declspec(dllexport) void SuiExecuteJs(int layerId, const char* jsUtf8);
__declspec(dllexport) void SuiCmdNavigate(int layerId, const char* urlUtf8);
__declspec(dllexport) void SuiSetCapture(int enabled);
__declspec(dllexport) int  SuiIsCaptureActive(void);
__declspec(dllexport) void SuiRegisterEventSink(void (*cb)(const char* utf8));

// ---- issue #2 exports (layer stack + content scheme) ----
// Creates a layer loading urlUtf8 (sentinelui://<plugin>/<path>, or https:// when
// the plugin opted into remote content). Returns the native layer id (0 on
// failure: budget exhausted / host not ready).
__declspec(dllexport) int  SuiCreateLayer(const char* urlUtf8);

// Closes a layer. Ids are never reused within a session.
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

// Event flow (unchanged shape, new body prefix):
//   helper -> pipe -> queue -> SuiPumpEvents -> sink
//   sink line: "<nativeLayerId>|<kind>:<body>" (e.g. "1|console:NUI_...").

#ifdef __cplusplus
}
#endif

#endif
