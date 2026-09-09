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

// Outbound event queue (writer flushes on the reader thread to keep it simple).
std::mutex g_outMutex;
std::vector<std::string> g_outQueue;

// Called on the reader thread for every command line received.
void (*g_onCommand)(const std::string&) = nullptr;

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
        static int s = 0;
        if (s++ == 0) SUI_LOG_INFO("IPC: first flush (%u bytes)", (unsigned)line.size());
        WriteFile(g_pipe, line.data(), (DWORD)line.size(), &written, nullptr);
    }
    FlushFileBuffers(g_pipe);
}

void ReaderLoop() {
    std::string acc;
    char buf[4096];
    while (!g_stop) {
        DWORD n = 0;
        // The reader may block waiting for commands — that is fine: outbound
        // events are flushed by the dedicated writer thread below.
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
            if (g_onCommand) g_onCommand(line);
        }
    }
}

void WriterLoop() {
    while (!g_stop) {
        FlushOut();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

}  // namespace

void SetCommandHandler(void (*fn)(const std::string&)) { g_onCommand = fn; }

void IpcSendEvent(const std::string& utf8Body) {
    if (g_pipe == INVALID_HANDLE_VALUE) return;
    std::lock_guard<std::mutex> lock(g_outMutex);
    std::string line = "E" + std::to_string(utf8Body.size()) + ":" + utf8Body + "\n";
    g_outQueue.push_back(std::move(line));
}

std::wstring StartIpc(const std::wstring& pipeName) {
    g_stop = false;
    // Reconnect loop: the ASI creates the pipe before spawning us, but retry anyway.
    for (int i = 0; i < 100 && g_pipe == INVALID_HANDLE_VALUE && !g_stop; ++i) {
        g_pipe = CreateFileW(pipeName.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                             OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
        if (g_pipe == INVALID_HANDLE_VALUE) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    if (g_pipe == INVALID_HANDLE_VALUE) return L"IPC connect failed";
    g_readerThread = std::thread(&ReaderLoop);
    g_writerThread = std::thread(&WriterLoop);
    return L"";
}

void StopIpc() {
    g_stop = true;
    if (g_pipe != INVALID_HANDLE_VALUE) {
        DisconnectNamedPipe(g_pipe);
        CloseHandle(g_pipe);
        g_pipe = INVALID_HANDLE_VALUE;
    }
    if (g_readerThread.joinable()) g_readerThread.join();
    if (g_writerThread.joinable()) g_writerThread.join();
}

}  // namespace sui_helper