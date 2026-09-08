#include "suilog.h"

#include <wchar.h>
#include <cstdio>
#include <cwchar>
#include <string>
#include <vector>

namespace sui_host {

namespace {
// Multi-level directory creation (CreateDirectoryW is single-level only).
bool EnsureDirectory(const std::wstring& path) {
    if (path.empty()) return false;
    std::wstring cur;
    for (wchar_t ch : path) {
        cur += ch;
        if (ch == L'\\' || ch == L'/') {
            if (cur.size() > 3) CreateDirectoryW(cur.c_str(), nullptr);
        }
    }
    CreateDirectoryW(path.c_str(), nullptr);
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}
}  // namespace

FILE* Log::file_ = nullptr;
CRITICAL_SECTION Log::cs_;
bool Log::csInit_ = false;

void Log::Initialize(const std::wstring& logDirectory, const std::wstring& filePrefix) {
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
    EnsureDirectory(dir);

    wchar_t buf[MAX_PATH];
    struct tm tmNow;
    __time64_t t64 = _time64(nullptr);
    _localtime64_s(&tmNow, &t64);
    wcsftime(buf, MAX_PATH, L"_%Y%m%d.log", &tmNow);
    std::wstring path = dir + L"\\" + filePrefix + buf;
    // BINARY append mode: the CRT "ccs=UTF-8" flavour produces a wide stream and
    // byte-oriented fprintf on it is undefined (CRT invalid-parameter -> failfast).
    // We write UTF-8 bytes ourselves.
    file_ = _wfopen(path.c_str(), L"ab");
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

    char body[2048];
    vsnprintf_s(body, _countof(body), _TRUNCATE, fmt, ap);

    std::string line = "[" + std::string(ts) + "] [" + std::string(level) + "] " + body + "\n";
    std::fwrite(line.data(), 1, line.size(), file_);
    LeaveCriticalSection(&cs_);
}

}  // namespace sui_host