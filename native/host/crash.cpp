#include "crash.h"

#include <windows.h>
#include <dbghelp.h>

#include <cwchar>

#pragma comment(lib, "dbghelp.lib")

namespace sui_host {

namespace {

// Fixed buffers only: the filter can run on a corrupted heap, so it must not
// allocate. Everything it needs is captured at install time.
wchar_t g_dumpDir[MAX_PATH * 2] = L"";
wchar_t g_dumpPrefix[64] = L"sentinelui_crash";
LPTOP_LEVEL_EXCEPTION_FILTER g_previous = nullptr;
volatile LONG g_installed = 0;

LONG WINAPI CrashFilter(EXCEPTION_POINTERS* info) {
    // One-shot: if the dump write itself faults, do not recurse.
    static volatile LONG busy = 0;
    if (InterlockedExchange(&busy, 1) == 0) {
        wchar_t path[MAX_PATH * 2 + 96];
        SYSTEMTIME st;
        GetLocalTime(&st);
        int n = swprintf_s(path, L"%s\\%s_%04d%02d%02d_%02d%02d%02d_pid%lu.dmp",
                           g_dumpDir, g_dumpPrefix, st.wYear, st.wMonth, st.wDay,
                           st.wHour, st.wMinute, st.wSecond, GetCurrentProcessId());
        if (n > 0) {
            CreateDirectoryW(g_dumpDir, nullptr);
            HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                      FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file != INVALID_HANDLE_VALUE) {
                MINIDUMP_EXCEPTION_INFORMATION mei = {};
                mei.ThreadId = GetCurrentThreadId();
                mei.ExceptionPointers = info;
                mei.ClientPointers = FALSE;
                MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file,
                                  (MINIDUMP_TYPE)(MiniDumpNormal | MiniDumpWithThreadInfo),
                                  info ? &mei : nullptr, nullptr, nullptr);
                CloseHandle(file);
            }
        }
    }
    if (g_previous) return g_previous(info);
    return EXCEPTION_CONTINUE_SEARCH;
}

}  // namespace

void InstallCrashHandler(const std::wstring& dumpDirectory, const wchar_t* dumpPrefix) {
    if (InterlockedCompareExchange(&g_installed, 1, 0) != 0) return;
    wcsncpy_s(g_dumpDir, dumpDirectory.c_str(), _TRUNCATE);
    size_t len = wcslen(g_dumpDir);
    if (len > 0 && g_dumpDir[len - 1] != L'\\' && g_dumpDir[len - 1] != L'/') {
        wcsncat_s(g_dumpDir, L"\\", _TRUNCATE);
    }
    if (dumpPrefix && *dumpPrefix) wcsncpy_s(g_dumpPrefix, dumpPrefix, _TRUNCATE);
    g_previous = SetUnhandledExceptionFilter(&CrashFilter);
}

}  // namespace sui_host
