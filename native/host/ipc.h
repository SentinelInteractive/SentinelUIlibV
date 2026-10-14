// SentinelUI ASI: IPC server to the helper process.
// Creates \\.\pipe\SentinelUI, accepts the helper, reads its event lines into a
// queue (drained by SuiPumpEvents on the game thread), and flushes ASI->helper
// command lines (input, exec-js, show/hide, viewport...).

#pragma once

#include <string>

namespace sui_host {

bool StartIpcServer();          // create pipe + accept thread (call BEFORE spawning the helper)
void StopIpcServer();

// Queue an ASI->helper command line (thread-safe; drained by the writer thread).
void SendCommand(const std::string& line);

// Monotonic helper-connection counter; increments every time a helper connects.
// Lets hook code re-send state (viewport) after a helper restart.
int IpcConnectionEpoch(void);

// Pop the oldest helper->ASI event line (game thread; SuiPumpEvents).
bool EventQueuePop(std::string& out);

// Managed callback for drained events: invoked from SuiPumpEvents on the caller thread.
void SetEventSink(void (*cb)(const char* utf8));

// Drain all queued events through the registered sink (called by SuiPumpEvents).
void DrainEventsToSink();

// Per-client event queues (host API v4, cross-AppDomain delivery).
// Each AppDomain registers once; every inbound event line is copied into every
// client queue (bounded) by whichever pump runs first. PumpEventClient drains
// only the calling client's queue on the calling thread, so each domain can
// dispatch to its own delegates without cross-domain calls.
unsigned long long RegisterEventClient();
void UnregisterEventClient(unsigned long long token);
void PumpEventClient(unsigned long long token, void (*cb)(const char* utf8));

}  // namespace sui_host