#include "suilog.h"

#include <wchar.h>
#include <cstdio>
#include <cwchar>
#include <vector>

namespace sui_host {

FILE* Log::file_ = nullptr;
CRITICAL_SECTION Log::cs_;
bool Log::csInit_ = false;

void Log::Initialize(const std::wstring& logDirectory) {
    if (!csInit_) {
        InitializeCriticalSection(&cs_);
        csInit_ = true;
    }
    EnterCriticalSection(&cs_);
    if (file_) {
        LeaveCriticalSection(&cs_);
        return;
    }

    std::wstring dir = logDirectory;
    if (!dir.empty() && (dir.back() == L'\\' || dir.back() == L'/')) dir.pop_back();
    CreateDirectoryW(dir.c_str(), nullptr);

    wchar_t buf[MAX_PATH];
    struct tm tmNow;
    __time64_t t64 = _time64(nullptr);
    _localtime64_s(&tmNow, &t64);
    wcsftime(buf, MAX_PATH, L"sentinelui_host_%Y%m%d.log", &tmNow);
    std::wstring path = dir + L"\\" + buf;
    file_ = _wfopen(path.c_str(), L"a, ccs=UTF-8");
    if (file_) setvbuf(file_, nullptr, _IONBF, 0);
    LeaveCriticalSection(&cs_);
}

void Log::Write(const char* level, const char* fmt, va_list ap) {
    if (!csInit_) return;
    EnterCriticalSection(&cs_);
    if (!file_) {
        LeaveCriticalSection(&cs_);
        return;
    }

    char ts[64];
    SYSTEMTIME st;
    GetLocalTime(&st);
    std::snprintf(ts, sizeof(ts), "%04d-%02d-%02d %02d:%02d:%02d.%03d",
                  st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);

    std::fprintf(file_, "[%s] [%s] ", ts, level);
    std::vfprintf(file_, fmt, ap);
    std::fprintf(file_, "\n");
    LeaveCriticalSection(&cs_);
}

}  // namespace sui_host