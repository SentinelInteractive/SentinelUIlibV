// SentinelUI ASI — IPC server implementation.

#include "ipc.h"
#include "suilog.h"

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace sui_host {

namespace {

HANDLE g_pipe = INVALID_HANDLE_VALUE;
std::thread g_thread;
std::thread g_writerThread;
std::atomic<bool> g_stop{false};

std::mutex g_inMutex;
std::vector<std::string> g_inQueue;    // helper->ASI events (read by pump)
std::mutex g_outMutex;
std::vector<std::string> g_outQueue;   // ASI->helper commands (written by server thread)

void (*g_sink)(const char*) = nullptr;

void FlushOut() {
    std::vector<std::string> batch;
    {
        std::lock_guard<std::mutex> lock(g_outMutex);
        if (g_outQueue.empty()) return;
        batch.swap(g_outQueue);
    }
    if (g_pipe == INVALID_HANDLE_VALUE) return;
    for (const auto& line : batch) {
        DWORD written = 0;
        WriteFile(g_pipe, line.data(), (DWORD)line.size(), &written, nullptr);
    }
    FlushFileBuffers(g_pipe);
}

void AcceptLoop() {
    while (!g_stop) {
        // One connection at a time; reconnect when the helper disappears.
        g_pipe = CreateNamedPipeW(L"\\\\.\\pipe\\SentinelUI",
                                  PIPE_ACCESS_DUPLEX,
                                  PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                                  PIPE_UNLIMITED_INSTANCES, 65536, 65536, 0, nullptr);
        if (g_pipe == INVALID_HANDLE_VALUE) {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            continue;
        }
        if (!ConnectNamedPipe(g_pipe, nullptr) &&
            GetLastError() != ERROR_PIPE_CONNECTED) {
            CloseHandle(g_pipe);
            g_pipe = INVALID_HANDLE_VALUE;
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            continue;
        }
        SUI_LOG_INFO("IPC: helper connected.");

        std::string acc;
        char buf[4096];
        while (!g_stop) {
            DWORD n = 0;
            // May block until the helper writes — fine; outbound commands are
            // flushed by the dedicated writer thread.
            if (!ReadFile(g_pipe, buf, sizeof(buf), &n, nullptr) || n == 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }
            acc.append(buf, n);
            size_t nl;
            while ((nl = acc.find('\n')) != std::string::npos) {
                std::string line = acc.substr(0, nl);
                acc.erase(0, nl + 1);
                if (line.empty()) continue;
                {
                    std::lock_guard<std::mutex> lock(g_inMutex);
                    g_inQueue.push_back(line);
                }
                static int s = 0;
                if (s++ == 0) SUI_LOG_INFO("IPC: first event recv (%u bytes)", (unsigned)line.size());
            }
        }
        CloseHandle(g_pipe);
        g_pipe = INVALID_HANDLE_VALUE;
        SUI_LOG_INFO("IPC: helper disconnected.");
    }
}

void WriterLoop() {
    while (!g_stop) {
        FlushOut();
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
    if (g_pipe != INVALID_HANDLE_VALUE) {
        CloseHandle(g_pipe);
        g_pipe = INVALID_HANDLE_VALUE;
    }
    if (g_thread.joinable()) g_thread.join();
    if (g_writerThread.joinable()) g_writerThread.join();
}

void SendCommand(const std::string& line) {
    std::lock_guard<std::mutex> lock(g_outMutex);
    g_outQueue.push_back(line);
}

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