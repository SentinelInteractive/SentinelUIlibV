// SentinelUI ASI — IPC server implementation.
//
// Transport lesson (verified offline + in-game): named-pipe I/O must use real
// OVERLAPPED structures on the server handle. Blocking ReadFile/WriteFile
// without OVERLAPPED on a FILE_FLAG_OVERLAPPED handle "may incorrectly report
// completion" and was observed to leave bytes stuck in the pipe after the first
// exchange. A synchronous server handle with a concurrent writer thread also
// stalls its blocking reads. Hence: every operation here is OVERLAPPED with an
// event and a bounded wait, and every wait polls g_stop so shutdown can always
// join the threads (a thread blocked in ConnectNamedPipe/ReadFile without an
// OVERLAPPED cannot be released by CloseHandle from another thread — verified).

#include "ipc.h"
#include "suilog.h"

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace sui_host {

namespace {

// Pipe handle shared between the accept thread (writes it) and the writer/stop
// paths (read it). Must be atomic: the writer flushes ASI->helper commands on a
// separate thread that may race a reconnect.
std::atomic<HANDLE> g_pipe{INVALID_HANDLE_VALUE};
std::thread g_thread;
std::thread g_writerThread;
std::atomic<bool> g_stop{false};

std::mutex g_inMutex;
std::vector<std::string> g_inQueue;    // helper->ASI events (read by pump)
std::mutex g_outMutex;
std::vector<std::string> g_outQueue;   // ASI->helper commands (drained by writer thread)
std::atomic<int> g_epoch{0};           // bumped on every helper connect

void (*g_sink)(const char*) = nullptr;

HANDLE CreateWaiter(HANDLE* outEvent) {
    // Auto-reset: waited-on once per pending operation; a manual-reset event
    // stays signaled and makes later waits return before completion.
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (outEvent) *outEvent = ev;
    return ev;
}

// Log the command type once per session (keeps the hot M path quiet).
void LogFirstQueued(const std::string& line) {
    if (line.empty()) return;
    unsigned char c = (unsigned char)line[0];
    static bool s_seen[256] = {};
    if (s_seen[c]) return;
    s_seen[c] = true;
    std::string body = line;
    if (!body.empty() && body.back() == '\n') body.pop_back();
    SUI_LOG_INFO("cmd queued: first '%c' (%u bytes): %s", (char)c, (unsigned)line.size(),
                 body.c_str());
}

// Requeue [it, end) at the FRONT of the out queue (order preserved) so a
// reconnect can deliver them. Returns true when anything was requeued.
bool Requeue(const std::vector<std::string>::const_iterator& it,
             const std::vector<std::string>& batch) {
    std::lock_guard<std::mutex> lock(g_outMutex);
    if (it == batch.end()) return false;
    g_outQueue.insert(g_outQueue.begin(), it, batch.end());
    return true;
}

void FlushOut(HANDLE pipe) {
    // Never drop commands just because the helper is (re)connecting.
    std::vector<std::string> batch;
    {
        std::lock_guard<std::mutex> lock(g_outMutex);
        if (g_outQueue.empty()) return;
        batch.swap(g_outQueue);
    }

    static HANDLE s_ev = CreateWaiter(nullptr);
    static bool s_firstFlush = true;
    if (s_firstFlush) {
        s_firstFlush = false;
        size_t bytes = 0;
        for (const auto& line : batch) bytes += line.size();
        SUI_LOG_INFO("IPC: first outbound flush (%u lines, %u bytes).",
                     (unsigned)batch.size(), (unsigned)bytes);
    }

    OVERLAPPED wo = {};
    wo.hEvent = s_ev;
    for (auto it = batch.begin(); it != batch.end(); ++it) {
        DWORD written = 0;
        const std::string& line = *it;
        bool failed = false;
        DWORD err = 0;
        if (!WriteFile(pipe, line.data(), (DWORD)line.size(), &written, &wo)) {
            err = GetLastError();
            if (err == ERROR_IO_PENDING) {
                DWORD w = WaitForSingleObject(wo.hEvent, 2000);
                if (w == WAIT_OBJECT_0) {
                    if (GetOverlappedResult(pipe, &wo, &written, FALSE)) continue;
                    err = GetLastError();
                } else if (w == WAIT_TIMEOUT) {
                    err = WAIT_TIMEOUT;
                }
            }
            failed = true;
        }
        if (failed) {
            static std::atomic<DWORD> s_lastErr{0};
            if (s_lastErr.exchange(err) != err)
                SUI_LOG_ERROR("IPC write failed (%lu) — command requeued for reconnect.", err);
            CancelIoEx(pipe, &wo);
            // Retire the cancelled OVERLAPPED before it goes out of scope / is
            // reused (a pending op writing into freed stack memory corrupts).
            WaitForSingleObject(wo.hEvent, 2000);
            GetOverlappedResult(pipe, &wo, &written, FALSE);
            Requeue(it, batch);
            break;
        }
    }
}

void AcceptLoop() {
    while (!g_stop) {
        // One connection at a time; recreate + reconnect when the helper drops.
        // Server handle is overlapped: every I/O op below passes a real
        // OVERLAPPED, and every wait polls g_stop (bounded), so shutdown can
        // always join this thread.
        HANDLE pipe = CreateNamedPipeW(L"\\\\.\\pipe\\SentinelUI",
                                       PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
                                       PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                                       PIPE_UNLIMITED_INSTANCES, 65536, 65536, 0, nullptr);
        if (pipe == INVALID_HANDLE_VALUE) {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            continue;
        }
        HANDLE coEv = CreateWaiter(nullptr);
        OVERLAPPED co = {};
        co.hEvent = coEv;

        bool connected = false;
        bool connectPending = false;
        for (;;) {
            if (g_stop) break;
            if (!connectPending) {
                if (ConnectNamedPipe(pipe, &co)) { connected = true; break; }
                DWORD err = GetLastError();
                if (err == ERROR_PIPE_CONNECTED) { connected = true; break; }  // raced in
                if (err != ERROR_IO_PENDING) break;                            // unusable instance
                connectPending = true;
            }
            // Still pending: wait (bounded) and NEVER re-issue while pending —
            // overlapping a second ConnectNamedPipe on the same OVERLAPPED is
            // undefined behaviour.
            if (WaitForSingleObject(co.hEvent, 250) == WAIT_OBJECT_0) {
                DWORD n = 0;
                connected = GetOverlappedResult(pipe, &co, &n, FALSE) != FALSE;
                break;
            }
        }
        if (g_stop || !connected) {
            if (pipe != INVALID_HANDLE_VALUE) CloseHandle(pipe);
            CloseHandle(coEv);
            continue;
        }
        CloseHandle(coEv);

        g_pipe.store(pipe);
        ++g_epoch;
        SUI_LOG_INFO("IPC: helper connected.");

        // Event loop: overlapped reads with a bounded wait (250ms) so g_stop and
        // disconnects are always observed.
        HANDLE roEv = CreateWaiter(nullptr);
        OVERLAPPED ro = {};
        ro.hEvent = roEv;
        std::string acc;
        char buf[4096];
        bool broken = false;
        bool readPending = false;
        for (;;) {
            if (g_stop) break;
            DWORD n = 0;
            if (!readPending) {
                if (ReadFile(pipe, buf, sizeof(buf), &n, &ro)) {
                    // Completed synchronously; fall through to buffering below.
                } else {
                    DWORD err = GetLastError();
                    if (err != ERROR_IO_PENDING) { broken = true; break; }
                    readPending = true;
                }
            }
            if (readPending) {
                // Bounded wait so g_stop/disconnects are observed; keep waiting on
                // timeout — re-issuing ReadFile over a pending OVERLAPPED corrupts data.
                DWORD w = WaitForSingleObject(ro.hEvent, 250);
                if (w == WAIT_TIMEOUT) continue;
                if (w != WAIT_OBJECT_0) { broken = true; break; }
                if (!GetOverlappedResult(pipe, &ro, &n, FALSE)) { broken = true; break; }
                readPending = false;
            }
            if (n == 0) continue;
            acc.append(buf, n);
            size_t nl;
            while ((nl = acc.find('\n')) != std::string::npos) {
                std::string line = acc.substr(0, nl);
                acc.erase(0, nl + 1);
                if (line.empty()) continue;
                // Event envelope: "E<len>:<body>" — unwrap so the sink/managed side
                // only ever sees "<layerId>|<kind>:<body>".
                if (line[0] == 'E') {
                    size_t colon = line.find(':');
                    if (colon != std::string::npos) {
                        int len = atoi(line.substr(1, colon - 1).c_str());
                        if (len >= 0 && colon + 1 + (size_t)len <= line.size())
                            line = line.substr(colon + 1, (size_t)len);
                    }
                }
                {
                    std::lock_guard<std::mutex> lock(g_inMutex);
                    g_inQueue.push_back(line);
                }
                static int s = 0;
                if (s++ == 0)
                    SUI_LOG_INFO("IPC: first event recv (%u bytes)", (unsigned)line.size());
            }
        }
        CloseHandle(roEv);
        g_pipe.store(INVALID_HANDLE_VALUE);
        if (broken) CloseHandle(pipe);
        SUI_LOG_INFO("IPC: helper disconnected.");
    }
}

void WriterLoop() {
    while (!g_stop) {
        HANDLE pipe = g_pipe.load();
        if (pipe != INVALID_HANDLE_VALUE) FlushOut(pipe);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

}  // namespace

bool StartIpcServer() {
    g_stop = false;
    g_thread = std::thread(&AcceptLoop);
    g_writerThread = std::thread(&WriterLoop);
    return true;
}

void StopIpcServer() {
    g_stop = true;
    HANDLE pipe = g_pipe.exchange(INVALID_HANDLE_VALUE);
    if (pipe != INVALID_HANDLE_VALUE) {
        CancelIoEx(pipe, nullptr);
        CloseHandle(pipe);
    }
    if (g_thread.joinable()) g_thread.join();
    if (g_writerThread.joinable()) g_writerThread.join();
}

void SendCommand(const std::string& line) {
    std::lock_guard<std::mutex> lock(g_outMutex);
    LogFirstQueued(line);
    g_outQueue.push_back(line);
}

int IpcConnectionEpoch(void) { return g_epoch.load(); }

bool EventQueuePop(std::string& out) {
    std::lock_guard<std::mutex> lock(g_inMutex);
    if (g_inQueue.empty()) return false;
    out = std::move(g_inQueue.front());
    g_inQueue.erase(g_inQueue.begin());
    return true;
}

void SetEventSink(void (*cb)(const char* utf8)) { g_sink = cb; }

namespace {
void RunSink(const std::string& line) { if (g_sink) g_sink(line.c_str()); }
}  // namespace

void DrainEventsToSink() {
    for (;;) {
        std::string line;
        if (!EventQueuePop(line)) break;
        RunSink(line);
    }
}

}  // namespace sui_host