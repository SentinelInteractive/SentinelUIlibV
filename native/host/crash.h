// SentinelUI — process-local minidump capture (issue #6 crash-capture groundwork).
//
// Installs an unhandled-exception filter that writes a .dmp next to the logs
// (<logs dir>\<prefix>_<yyyymmdd_hhmmss>_pid<pid>.dmp) and then chains to the
// previous filter, so RPH's own handler (if any) still sees the exception.
// The filter deliberately does NOT touch the log (the faulting thread may hold
// the log's lock) and uses only fixed buffers (no CRT heap allocation).

#pragma once
#ifndef SENTINEL_HOST_CRASH_H
#define SENTINEL_HOST_CRASH_H

#include <string>

namespace sui_host {

// Safe to call repeatedly; only the first call installs. The log directory is
// created if missing (Log::Initialize normally already did that).
void InstallCrashHandler(const std::wstring& dumpDirectory, const wchar_t* dumpPrefix);

}  // namespace sui_host

#endif
