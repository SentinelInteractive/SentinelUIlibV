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

// P1 gate additions (unused by the managed skeleton yet, exported for the ABI):
//   int  SuiCreateLayer(const wchar_t* urlOrHtml)
//   void SuiCloseLayer(int layerId)
//   void SuiSetLayerVisible(int layerId, int visible)
//   void SuiSetLayerFocus(int layerId, int keyboard, int mouse)
//   void SuiExecuteJs(int layerId, const char* jsUtf8)
//   void SuiSendData(int layerId, const char* jsonEnvelopeUtf8)
//   uint64_t SuiRegisterEventSink(void* cb, int* outEpoch)
//   void SuiUnregisterEventSink(uint64_t token)

#ifdef __cplusplus
}
#endif

#endif