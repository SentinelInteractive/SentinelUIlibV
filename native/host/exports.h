// SentinelUI native host — exported C API contract (v2).
// MUST stay in sync with managed/SentinelUI.API/NativeHost.cs.

#pragma once
#ifndef SENTINEL_HOST_EXPORTS_H
#define SENTINEL_HOST_EXPORTS_H

#include <stdint.h>

#define SUI_API_VERSION 2

#ifdef __cplusplus
extern "C" {
#endif

// Returns the export-table API version. First call any plugin makes.
__declspec(dllexport) int SuiGetApiVersion(void);

// Drains host event queues. MUST be called on the game thread (per-frame).
__declspec(dllexport) void SuiPumpEvents(void);

// Graceful host shutdown. Only called from the RPH Bootstrap unload path.
__declspec(dllexport) void SuiShutdown(void);

// P2 additions (issue #1 — additive to v2):
//   void   SuiCmdVisible(int layerId, int visible)     // layerId reserved (1 layer for now)
//   void   SuiCmdFocus(int layerId, int keyboard, int mouse)
//   void   SuiExecuteJs(int layerId, const char* jsUtf8)
//   void   SuiCmdNavigate(int layerId, const char* urlUtf8)
//   void   SuiSetCapture(int enabled)
//   int    SuiIsCaptureActive(void)
//   void   SuiRegisterEventSink(void (*cb)(const char* utf8))
//   (event flow: helper -> pipe -> queue -> SuiPumpEvents -> sink)

// Issue #1 exports (implemented in host.cpp):
__declspec(dllexport) void SuiCmdVisible(int layerId, int visible);
__declspec(dllexport) void SuiCmdFocus(int layerId, int keyboard, int mouse);
__declspec(dllexport) void SuiExecuteJs(int layerId, const char* jsUtf8);
__declspec(dllexport) void SuiCmdNavigate(int layerId, const char* urlUtf8);
__declspec(dllexport) void SuiSetCapture(int enabled);
__declspec(dllexport) int  SuiIsCaptureActive(void);
__declspec(dllexport) void SuiRegisterEventSink(void (*cb)(const char* utf8));

#ifdef __cplusplus
}
#endif

#endif