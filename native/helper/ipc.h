// SentinelUI Helper — IPC client to the in-game ASI.
// One named pipe (\\.\pipe\SentinelUI, server = ASI). Client thread reads
// commands; a small writer sends event lines back.

#pragma once
#ifndef SENTINEL_HELPER_IPC_H
#define SENTINEL_HELPER_IPC_H

#include <windows.h>
#include <string>

namespace sui_helper {

// Returns an error string (empty on success).
std::wstring StartIpc(const std::wstring& pipeName);

void StopIpc();

// Sets the per-command-line callback (reader thread).
void SetCommandHandler(void (*fn)(const std::string&));

// True while the ASI's pipe is connected (false before connect / after a drop).
bool IpcConnected();

// Queue an event line to the ASI ('E<len>:<bytes>'). Async, safe from any thread.
void IpcSendEvent(const std::string& utf8Body);

}  // namespace sui_helper

#endif