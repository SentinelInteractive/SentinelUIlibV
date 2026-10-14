// SentinelUI: process-local minidump capture.
//
// Installs an unhandled-exception filter that writes a .dmp next to the logs
// (<logs dir>\<prefix>_<yyyymmdd_hhmmss>_pid<pid>.dmp) and then chains to the
// previous filter, so RPH's own handler (if any) still sees the exception.
// The filter must not touch the log (the faulting thread may hold its lock) and
// uses fixed buffers only (no CRT heap allocation).

#pragma once

#include <string>

namespace sui_host {

// Safe to call repeatedly; only the first call installs. The log directory is
// created if missing (Log::Initialize normally already did that).
void InstallCrashHandler(const std::wstring& dumpDirectory, const wchar_t* dumpPrefix);

}  // namespace sui_host
