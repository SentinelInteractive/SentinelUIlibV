// SentinelUI host-side logger. Own file (SentinelUI\logs\sentinelui_host_*.log) —
// NEVER spam RagePluginHook.log. Same policy as the managed side.

#pragma once
#ifndef SENTINEL_HOST_SUILOG_H
#define SENTINEL_HOST_SUILOG_H

#include <windows.h>
#include <cstdio>
#include <cstdarg>
#include <ctime>
#include <string>

namespace sui_host {

class Log {
public:
    // Call once with the absolute logs directory. Safe to call repeatedly.
    static void Initialize(const std::wstring& logDirectory);

    static void Write(const char* level, const char* fmt, ...);

    static void Info(const char* fmt, ...) {
        va_list ap; va_start(ap, fmt); Write("INFO ", fmt, ap); va_end(ap);
    }

    static void Warn(const char* fmt, ...) {
        va_list ap; va_start(ap, fmt); Write("WARN ", fmt, ap); va_end(ap);
    }

    static void Error(const char* fmt, ...) {
        va_list ap; va_start(ap, fmt); Write("ERROR", fmt, ap); va_end(ap);
    }

private:
    static void Write(const char* level, const char* fmt, va_list ap);

    static FILE* file_;
    static CRITICAL_SECTION cs_;
    static bool csInit_;
};

#define SUI_LOG_INFO(...)  ::sui_host::Log::Info(__VA_ARGS__)
#define SUI_LOG_WARN(...)  ::sui_host::Log::Warn(__VA_ARGS__)
#define SUI_LOG_ERROR(...) ::sui_host::Log::Error(__VA_ARGS__)

}  // namespace sui_host

#endif