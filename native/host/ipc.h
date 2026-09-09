// SentinelUI ASI — IPC server to the helper process.
// Creates \\.\pipe\SentinelUI, accepts the helper, reads its event lines into a
// queue (drained by SuiPumpEvents on the game thread), and flushes ASI->helper
// command lines (input, exec-js, show/hide, viewport...).

#pragma once
#ifndef SENTINEL_HOST_IPC_H
#define SENTINEL_HOST_IPC_H

#include <string>

namespace sui_host {

bool StartIpcServer();          // create pipe + accept thread (call BEFORE spawning the helper)
void StopIpcServer();

// Queue an ASI->helper command line (thread-safe; flushed by the server thread).
void SendCommand(const std::string& line);

// Pop the oldest helper->ASI event line (game thread; SuiPumpEvents).
bool EventQueuePop(std::string& out);

// Managed callback for drained events: invoked from SuiPumpEvents on the caller thread.
void SetEventSink(void (*cb)(const char* utf8));

// Drain all queued events through the registered sink (called by SuiPumpEvents).
void DrainEventsToSink();

}  // namespace sui_host

#endif