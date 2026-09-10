// SentinelUI Helper — IPC client implementation.

#include "ipc.h"
#include "suilog.h"

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace sui_helper {

namespace {

HANDLE g_pipe = INVALID_HANDLE_VALUE;
std::thread g_readerThread;
std::thread g_writerThread;
std::atomic<bool> g_stop{false};
std::atomic<bool> g_connected{false};

// Outbound event queue (writer flushes on the reader thread to keep it simple).
std::mutex g_outMutex;
std::vector<std::string> g_outQueue;

// Called on the reader thread for every command line received.
void (*g_onCommand)(const std::string&) = nullptr;

// OVERLAPPED write helper: WriteFile with lpOverlapped=NULL on an overlapped
// handle is documented to misreport completion and left events stuck in the
// pipe in practice — always pass a real OVERLAPPED + event here.
void WriteLineBlocking(HANDLE pipe, const std::string& line, OVERLAPPED& wo) {
    DWORD written = 0;
    if (WriteFile(pipe, line.data(), (DWORD)line.size(), &written, &wo)) return;
    if (GetLastError() != ERROR_IO_PENDING) return;
    if (WaitForSingleObject(wo.hEvent, 2000) == WAIT_OBJECT_0) {
        GetOverlappedResult(pipe, &wo, &written, FALSE);
        return;
    }
    CancelIoEx(pipe, &wo);
    // Wait for the cancellation to retire the OVERLAPPED before it is reused
    // by the next write (reuse while pending corrupts the stream).
    WaitForSingleObject(wo.hEvent, 2000);
    GetOverlappedResult(pipe, &wo, &written, FALSE);
}

void FlushOut() {
    std::vector<std::string> batch;
    {
        std::lock_guard<std::mutex> lock(g_outMutex);
        if (g_outQueue.empty()) return;
        batch.swap(g_outQueue);
    }
    // No pipe (reconnecting): keep the batch for the next connection.
    if (g_pipe == INVALID_HANDLE_VALUE) {
        std::lock_guard<std::mutex> lock(g_outMutex);
        g_outQueue.insert(g_outQueue.begin(), batch.begin(), batch.end());
        return;
    }
    // Auto-reset event: each pending write gets its own wait.
    static HANDLE s_ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    static OVERLAPPED s_wo = {};     // single writer thread: safe to reuse
    s_wo.hEvent = s_ev;
    for (const auto& line : batch) {
        static int s = 0;
        if (s++ == 0) SUI_LOG_INFO("IPC: first flush (%u bytes)", (unsigned)line.size());
        WriteLineBlocking(g_pipe, line, s_wo);
    }
}

void ProcessLines(std::string& acc) {
    size_t nl;
    while ((nl = acc.find('\n')) != std::string::npos) {
        std::string line = acc.substr(0, nl);
        acc.erase(0, nl + 1);
        if (line.empty()) continue;
        if (g_onCommand) g_onCommand(line);
    }
}

void ReaderLoop() {
    // Real OVERLAPPED read with a bounded wait: ReadFile with lpOverlapped=NULL
    // on an overlapped handle is documented to misreport completion and was
    // observed to leave commands sitting in the pipe (in-game + probe flakes).
    // Auto-reset event + a pending flag: a timeout keeps the outstanding read
    // alive (re-issuing over a pending OVERLAPPED corrupts the command stream).
    std::string acc;
    char buf[4096];
    OVERLAPPED ov = {};
    ov.hEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    bool pending = false;
    while (!g_stop) {
        if (!pending) {
            DWORD n = 0;
            if (ReadFile(g_pipe, buf, sizeof(buf), &n, &ov)) {
                if (n > 0) {
                    acc.append(buf, n);
                    ProcessLines(acc);
                }
                continue;  // completed synchronously; re-issue
            }
            DWORD err = GetLastError();
            if (err == ERROR_IO_PENDING) {
                pending = true;
            } else {
                // Disconnected or transient failure: retry (bounded below by g_stop).
                std::this_thread::sleep_for(std::chrono::milliseconds(25));
                continue;
            }
        }
        DWORD w = WaitForSingleObject(ov.hEvent, 200);
        if (w == WAIT_TIMEOUT) continue;   // still connected; keep waiting
        pending = false;
        DWORD n = 0;
        if (!GetOverlappedResult(g_pipe, &ov, &n, FALSE)) {
            g_connected = false;           // broken pipe; watchdog decides to exit
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
            continue;
        }
        g_connected = true;
        if (n > 0) {
            acc.append(buf, n);
            ProcessLines(acc);
        }
    }
    CancelIoEx(g_pipe, &ov);
    CloseHandle(ov.hEvent);
}

void WriterLoop() {
    while (!g_stop) {
        FlushOut();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

}  // namespace

void SetCommandHandler(void (*fn)(const std::string&)) { g_onCommand = fn; }

bool IpcConnected() { return g_connected.load(); }

void IpcSendEvent(const std::string& utf8Body) {
    if (g_pipe == INVALID_HANDLE_VALUE) return;
    std::lock_guard<std::mutex> lock(g_outMutex);
    std::string line = "E" + std::to_string(utf8Body.size()) + ":" + utf8Body + "\n";
    g_outQueue.push_back(std::move(line));
}

std::wstring StartIpc(const std::wstring& pipeName) {
    g_stop = false;
    // Reconnect loop: the ASI creates the pipe before spawning us, but retry anyway.
    // NOTE: overlapped handle is REQUIRED here: the reader thread waits for
    // commands while the dedicated writer thread writes on the same duplex
    // handle. On a synchronous handle a concurrent blocking write CANCELS the
    // outstanding read (verified experimentally) and the command path dies.
    // Both loops pass real OVERLAPPED structs (see ReaderLoop/WriteLineBlocking).
    for (int i = 0; i < 100 && g_pipe == INVALID_HANDLE_VALUE && !g_stop; ++i) {
        g_pipe = CreateFileW(pipeName.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                             OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
        if (g_pipe == INVALID_HANDLE_VALUE) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    if (g_pipe == INVALID_HANDLE_VALUE) return L"IPC connect failed";
    g_connected = true;
    g_readerThread = std::thread(&ReaderLoop);
    g_writerThread = std::thread(&WriterLoop);
    return L"";
}

void StopIpc() {
    g_stop = true;
    g_connected = false;
    if (g_pipe != INVALID_HANDLE_VALUE) {
        DisconnectNamedPipe(g_pipe);
        CloseHandle(g_pipe);
        g_pipe = INVALID_HANDLE_VALUE;
    }
    if (g_readerThread.joinable()) g_readerThread.join();
    if (g_writerThread.joinable()) g_writerThread.join();
}

}  // namespace sui_helper