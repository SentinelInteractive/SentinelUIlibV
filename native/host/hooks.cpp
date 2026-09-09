// SentinelUI host (ASI) — game hooks:
//   * IDXGISwapChain::Present / ResizeBuffers — function-level detours via MinHook.
//     Present uploads the latest shared-memory frame (written by the helper
//     process) to the game's D3D11 texture and composites it (game thread).
//   * WndProc subclass on the GTA V window — P1 hotkeys (F9 show/hide).
//     Input capture routing to the helper arrives with the IPC layer (P2).

#include "hooks.h"
#include "framesrc.h"
#include "ipc.h"
#include "renderer.h"
#include "suilog.h"

#include <MinHook.h>

#include <d3d11.h>
#include <atomic>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "d3d11.lib")

namespace sui_host {

extern Renderer g_renderer;    // defined in host.cpp
extern FrameSource g_frames;   // defined in host.cpp
extern bool g_visible;         // defined in host.cpp
extern bool g_framesReady;     // defined in host.cpp
extern bool g_capture;         // defined in host.cpp
void SetCaptureState(bool on); // defined in host.cpp

namespace {

HWND g_gameWindow = nullptr;
WNDPROC g_oldWndProc = nullptr;
bool g_hooked = false;

// Frame staging buffer (helper publishes up to 4K BGRA).
std::vector<uint8_t> g_frameBuf;
uint64_t g_lastGen = 0;

typedef HRESULT(WINAPI* PresentFn)(IDXGISwapChain*, UINT, UINT);
typedef HRESULT(WINAPI* ResizeFn)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
PresentFn g_origPresent = nullptr;
ResizeFn g_origResize = nullptr;

IDXGISwapChain* g_swapChain = nullptr;
bool g_viewportSent = false;

void SyncViewport() {
    if (g_viewportSent || !g_renderer.ready()) return;
    SendCommand(std::string("V") + std::to_string(g_renderer.width()) + " " +
                std::to_string(g_renderer.height()) + "\n");
    g_viewportSent = true;
}

void ToggleHotkeys(WPARAM wParam, LPARAM lParam) {
    if (wParam != VK_F9 && wParam != VK_F10) return;
    if (lParam & 0x40000000) return;  // auto-repeat
    if (wParam == VK_F9) {
        g_visible = !g_visible;
        SUI_LOG_INFO("F9: visibility -> %s", g_visible ? "visible" : "hidden");
    } else {
        SetCaptureState(!g_capture);
    }
}

// Client-area mouse coords from lParam (signed shorts).
inline int Cx(LPARAM l) { return (short)LOWORD(l); }
inline int Cy(LPARAM l) { return (short)HIWORD(l); }

// --- capture cursor handling ------------------------------------------------
// GTA V pins/hides the OS cursor while it owns the mouse, so WM_MOUSEMOVE never
// arrives and an OSR page sees no motion. While captured we unclip + show the
// cursor and poll GetCursorPos -> SendCommand (page cursor follows the mouse).
std::thread g_mouseThread;          // (started/stopped only from game thread)
std::atomic<bool> g_mouseStop{true};
std::atomic<int> g_lastCursorX{0};
std::atomic<int> g_lastCursorY{0};

void StartMousePolling() {
    if (!g_mouseThread.joinable()) {
        g_mouseStop = false;
        g_mouseThread = std::thread([]() {
            while (!g_mouseStop.load()) {
                POINT p;
                if (GetCursorPos(&p)) {
                    ScreenToClient(g_gameWindow, &p);
                    int px = g_lastCursorX.exchange(p.x);
                    int py = g_lastCursorY.exchange(p.y);
                    if (px != p.x || py != p.y) {
                        SendCommand(std::string("M") + std::to_string(p.x) + " " +
                                    std::to_string(p.y) + "\n");
                    }
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(8));
            }
        });
    }
}

void StopMousePolling() {
    g_mouseStop = true;
    if (g_mouseThread.joinable()) {
        g_mouseThread.join();
        g_mouseThread = std::thread();  // reset
    }
}

void InternalOnCaptureChanged(bool on) {
    if (!g_gameWindow) return;
    if (on) {
        ShowCursor(TRUE);
        RECT rc;
        if (GetClientRect(g_gameWindow, &rc)) {
            POINT tl = {rc.left, rc.top};
            ClientToScreen(g_gameWindow, &tl);
            rc.left = tl.x; rc.top = tl.y;
            rc.right += tl.x; rc.bottom += tl.y;
            ClipCursor(&rc);
        }
        StartMousePolling();
    } else {
        StopMousePolling();
        ClipCursor(nullptr);
        ShowCursor(FALSE);
    }
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN: {
            // Toggle hotkeys ONLY on keydown (keyup must not re-toggle).
            ToggleHotkeys(wParam, lParam);
            if (g_capture) {
                UINT vk = (UINT)wParam;
                UINT sc = (UINT)((lParam >> 16) & 0xff);
                bool ext = (lParam >> 24) & 1;
                SendCommand(std::string("K") + "1 " +
                            std::to_string(vk) + " " + std::to_string(sc) + " " +
                            (ext ? "1" : "0") + "\n");
                return 0;
            }
            break;
        }
        case WM_KEYUP:
        case WM_SYSKEYUP:
            if (g_capture) {
                UINT vk = (UINT)wParam;
                UINT sc = (UINT)((lParam >> 16) & 0xff);
                bool ext = (lParam >> 24) & 1;
                SendCommand(std::string("K") + "0 " +
                            std::to_string(vk) + " " + std::to_string(sc) + " " +
                            (ext ? "1" : "0") + "\n");
                return 0;
            }
            break;
        case WM_CHAR:
        case WM_SYSCHAR:
            if (g_capture) {
                SendCommand(std::string("U") + std::to_string((int)wParam) + "\n");
                return 0;
            }
            break;
        case WM_MOUSEMOVE:
            if (g_capture)
                SendCommand(std::string("M") + std::to_string(Cx(lParam)) + " " +
                            std::to_string(Cy(lParam)) + "\n");
            break;
        case WM_MOUSEWHEEL:
            if (g_capture) {
                int delta = (short)HIWORD(wParam);
                POINT p = {Cx(lParam), Cy(lParam)};
                ScreenToClient(hwnd, &p);
                SendCommand(std::string("W") + std::to_string(delta) + " " +
                            std::to_string(p.x) + " " + std::to_string(p.y) + "\n");
                return 0;
            }
            break;
        case WM_LBUTTONDOWN: case WM_LBUTTONUP:
        case WM_RBUTTONDOWN: case WM_RBUTTONUP:
        case WM_MBUTTONDOWN: case WM_MBUTTONUP:
            if (g_capture) {
                bool down = (msg == WM_LBUTTONDOWN || msg == WM_RBUTTONDOWN || msg == WM_MBUTTONDOWN);
                int button = (msg == WM_LBUTTONDOWN || msg == WM_LBUTTONUP) ? 0 :
                             (msg == WM_RBUTTONDOWN || msg == WM_RBUTTONUP) ? 2 : 1;
                SendCommand(std::string("B") + std::to_string(button) + " " +
                            (down ? "1" : "0") + " " + std::to_string(Cx(lParam)) + " " +
                            std::to_string(Cy(lParam)) + "\n");
                return 0;
            }
            break;
        case WM_SIZE:
            g_renderer.Invalidate();   // re-create texture on next Present
            break;
        case WM_KILLFOCUS:
            SUI_LOG_INFO("WM_KILLFOCUS (alt-tab).");
            break;
        default:
            break;
    }
    return CallWindowProcW(g_oldWndProc, hwnd, msg, wParam, lParam);
}

HRESULT WINAPI PresentHook(IDXGISwapChain* swapChain, UINT syncInterval, UINT flags) {
    // Lazy resource init + viewport sync from the very first Present, regardless
    // of frame availability (input scaling must exist even before first paint).
    if (g_swapChain != swapChain || !g_renderer.ready()) {
        g_swapChain = swapChain;
        if (g_renderer.EnsureResources(swapChain)) {
            g_viewportSent = false;
        }
    }
    SyncViewport();

    if (g_framesReady && g_visible && g_renderer.ready()) {
        if (g_frameBuf.empty()) g_frameBuf.resize(4096 * 2160 * 4u);
        uint64_t gen = g_lastGen;
        if (g_frames.Take(g_frameBuf.data(), g_frameBuf.size(), gen)) {
            g_renderer.UploadFrame(g_frameBuf.data(), (int)g_frames.width(), (int)g_frames.height());
            g_lastGen = gen;
        }
        g_renderer.DrawFrame(swapChain);
    }

    return g_origPresent(swapChain, syncInterval, flags);
}

HRESULT WINAPI ResizeBuffersHook(IDXGISwapChain* swapChain, UINT count, UINT w, UINT h,
                                 DXGI_FORMAT format, UINT flags) {
    HRESULT hr = g_origResize(swapChain, count, w, h, format, flags);
    g_renderer.Invalidate();
    g_viewportSent = false;
    return hr;
}

}  // namespace

// sui_host-scope wrapper (hooks.h declaration) -> anonymous helper.
void OnCaptureChanged(bool on) { InternalOnCaptureChanged(on); }

bool InstallHooks(HWND gameWindow) {
    if (g_hooked) return true;
    g_gameWindow = gameWindow;

    g_oldWndProc = (WNDPROC)SetWindowLongPtrW(g_gameWindow, GWLP_WNDPROC, (LONG_PTR)&WindowProc);
    if (!g_oldWndProc) {
        SUI_LOG_ERROR("SetWindowLongPtrW failed (%lu).", GetLastError());
        return false;
    }

    // Resolve Present/ResizeBuffers addresses via a dummy swapchain.
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"SentinelUIDummy";
    RegisterClassExW(&wc);
    HWND dummy = CreateWindowExW(0, L"SentinelUIDummy", L"dummy", WS_OVERLAPPED,
                                 0, 0, 64, 64, nullptr, nullptr, wc.hInstance, nullptr);

    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 1;
    sd.BufferDesc.Width = 64;
    sd.BufferDesc.Height = 64;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = dummy;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;

    ID3D11Device* dev = nullptr;
    IDXGISwapChain* chain = nullptr;
    D3D_FEATURE_LEVEL level;
    if (FAILED(D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                             D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                                             D3D11_SDK_VERSION, &sd, &chain, &dev, &level, nullptr))) {
        SUI_LOG_ERROR("Dummy swapchain failed; hooks aborted.");
        if (dummy) DestroyWindow(dummy);
        return false;
    }

    void** vtable = *(void***)chain;
    PresentFn presentAddr = (PresentFn)vtable[8];
    ResizeFn resizeAddr = (ResizeFn)vtable[13];

    if (dev) dev->Release();
    if (chain) chain->Release();
    if (dummy) DestroyWindow(dummy);

    if (MH_Initialize() != MH_OK) {
        SUI_LOG_ERROR("MH_Initialize failed.");
        return false;
    }
    MH_STATUS st = MH_CreateHook(presentAddr, &PresentHook, (void**)&g_origPresent);
    if (st == MH_OK) st = MH_CreateHook(resizeAddr, &ResizeBuffersHook, (void**)&g_origResize);
    if (st == MH_OK) st = MH_EnableHook(MH_ALL_HOOKS);
    if (st != MH_OK) {
        SUI_LOG_ERROR("MinHook install failed: %d", st);
        return false;
    }
    g_hooked = true;
    SUI_LOG_INFO("Hooks installed (Present/ResizeBuffers detoured, WndProc subclassed).");
    return true;
}

void RemoveHooks() {
    StopMousePolling();
    if (!g_hooked) return;
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();
    if (g_oldWndProc) {
        SetWindowLongPtrW(g_gameWindow, GWLP_WNDPROC, (LONG_PTR)g_oldWndProc);
        g_oldWndProc = nullptr;
    }
    g_hooked = false;
    SUI_LOG_INFO("Hooks removed.");
}

}  // namespace sui_host