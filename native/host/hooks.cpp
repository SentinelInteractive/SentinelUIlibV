// SentinelUI host (ASI) — game hooks:
//   * IDXGISwapChain::Present / ResizeBuffers — function-level detours via MinHook.
//     Present pumps every layer's shared-memory frames and composites the
//     visible stack (z order, premultiplied alpha) onto the backbuffer.
//   * WndProc subclass on the GTA V window — P1 hotkeys (F9 show/hide, F10
//     capture) + the issue #2 layer stack: captured input is routed per layer
//     (keyboard -> focused layer, mouse -> topmost non-click-through layer).

#include "hooks.h"
#include "framesrc.h"
#include "ipc.h"
#include "layers.h"
#include "renderer.h"
#include "suilog.h"

#include <MinHook.h>

#include <d3d11.h>
#include <atomic>
#include <map>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "d3d11.lib")

namespace sui_host {

extern Renderer g_renderer;    // defined in host.cpp
extern LayerTable g_layers;    // defined in host.cpp
extern bool g_visible;         // defined in host.cpp
extern bool g_framesReady;     // defined in host.cpp
extern bool g_capture;         // defined in host.cpp
void SetCaptureState(bool on); // defined in host.cpp
void ToggleDemoLayer();        // defined in host.cpp

namespace {

HWND g_gameWindow = nullptr;
WNDPROC g_oldWndProc = nullptr;
bool g_hooked = false;

// Keys currently forwarded to a page while captured (VK -> scan code, extended).
// Game-thread only: WndProc and capture transitions both run on the game thread.
std::map<UINT, std::pair<UINT, bool>> g_keysDown;

// Buttons currently forwarded while captured (0=left, 1=middle, 2=right; game thread
// only). A release that never reaches the page (focus loss/alt-tab/capture toggle
// while holding) would leave CEF believing the button is still down, blocking or
// mis-firing subsequent clicks — capture transitions synthesize ups for these.
bool g_mouseDown[3] = {false, false, false};

// Focus loss must not leave the page thinking a key is still held, and must not
// leave the game with its controls disabled. Synthesize key-ups, then release.
void ReleaseCapturedKeys() {
    if (g_keysDown.empty()) return;
    int id = g_layers.KeyTarget();
    if (id) {
        for (const auto& kv : g_keysDown) {
            SendCommand("K" + std::to_string(id) + " 0 " + std::to_string(kv.first) + " " +
                        std::to_string(kv.second.first) + " " +
                        (kv.second.second ? "1" : "0") + "\n");
        }
    }
    g_keysDown.clear();
}

void FocusLost() {
    if (!g_capture) return;
    SUI_LOG_INFO("capture released on focus loss.");
    ReleaseCapturedKeys();
    SetCaptureState(false);
}

// Frame staging buffer (shared scratch; helper publishes up to 4K BGRA per
// layer — each layer's newest frame is copied in and uploaded immediately).
std::vector<uint8_t> g_frameBuf;

typedef HRESULT(WINAPI* PresentFn)(IDXGISwapChain*, UINT, UINT);
typedef HRESULT(WINAPI* ResizeFn)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
PresentFn g_origPresent = nullptr;
ResizeFn g_origResize = nullptr;

IDXGISwapChain* g_swapChain = nullptr;
bool g_viewportSent = false;
std::atomic<int> g_lastIpcEpoch{0};

// Send the helper viewport on first Present, on every resize, and again after a
// helper reconnect (the new helper process starts at its default 1280x720).
void SyncViewport() {
    if (!g_renderer.ready()) return;
    int epoch = IpcConnectionEpoch();
    if (g_viewportSent && epoch == g_lastIpcEpoch.load()) return;
    g_viewportSent = true;
    g_lastIpcEpoch.store(epoch);
    SendCommand(std::string("V") + std::to_string(g_renderer.width()) + " " +
                std::to_string(g_renderer.height()) + "\n");
    // Re-assert the master visibility state after a helper (re)connect.
    SendCommand(g_visible ? "A1\n" : "A0\n");
    SUI_LOG_INFO("viewport sync -> %dx%d (ipc epoch %d)", g_renderer.width(),
                 g_renderer.height(), epoch);
}

bool ToggleHotkeys(WPARAM wParam, LPARAM lParam) {
    if (wParam != VK_F9 && wParam != VK_F10 && wParam != VK_F11) return false;
    if (lParam & 0x40000000) return true;  // auto-repeat: swallow, do not re-toggle
    if (wParam == VK_F9) {
        g_visible = !g_visible;
        SUI_LOG_INFO("F9: visibility -> %s", g_visible ? "visible" : "hidden");
        // Pause/resume all helper pages so a hidden overlay is truly idle and a
        // shown overlay repaints immediately. The host also force-uploads every
        // layer's resident frame on the next Present, so all layers composite
        // in the same frame (no per-layer stagger).
        SendCommand(g_visible ? "A1\n" : "A0\n");
    } else if (wParam == VK_F10) {
        SetCaptureState(!g_capture);
    } else {
        ToggleDemoLayer();  // F11: demo HUD layer (issue #2 acceptance)
    }
    return true;   // hotkeys are ours: never forward them to the page or the game
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
                        // Route to the topmost interactive (non-click-through) layer.
                        int id = g_layers.MouseTarget();
                        if (id) {
                            SendCommand("M" + std::to_string(id) + " " + std::to_string(p.x) +
                                        " " + std::to_string(p.y) + "\n");
                        }
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

// Synthesize button-ups for anything still held when capture ends (F10 toggle,
// alt-tab/focus loss, shutdown). Without this CEF keeps the button state and the
// page stops reacting to clicks until a matching up arrives.
void ReleaseCapturedMouseButtons() {
    if (!g_mouseDown[0] && !g_mouseDown[1] && !g_mouseDown[2]) return;
    int id = g_layers.MouseTarget();
    int x = g_lastCursorX.load();
    int y = g_lastCursorY.load();
    for (int b = 0; b < 3; ++b) {
        if (!g_mouseDown[b]) continue;
        g_mouseDown[b] = false;
        if (id) {
            SendCommand("B" + std::to_string(id) + " " + std::to_string(b) + " 0 " +
                        std::to_string(x) + " " + std::to_string(y) + "\n");
        }
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
        ReleaseCapturedMouseButtons();
        StopMousePolling();
        ClipCursor(nullptr);
        ShowCursor(FALSE);
    }
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN: {
            // Toggle hotkeys ONLY on keydown (keyup must not re-toggle), and never
            // forward them to the page or the game.
            if (ToggleHotkeys(wParam, lParam)) return 0;
            if (g_capture) {
                // Keyboard goes to the focused layer (else topmost interactive).
                int id = g_layers.KeyTarget();
                if (id) {
                    UINT vk = (UINT)wParam;
                    UINT sc = (UINT)((lParam >> 16) & 0xff);
                    bool ext = (lParam >> 24) & 1;
                    SendCommand("K" + std::to_string(id) + " 1 " + std::to_string(vk) + " " +
                                std::to_string(sc) + " " + (ext ? "1" : "0") + "\n");
                    g_keysDown[vk] = std::make_pair(sc, ext);
                    return 0;
                }
            }
            break;
        }
        case WM_KEYUP:
        case WM_SYSKEYUP:
            if (wParam == VK_F9 || wParam == VK_F10 || wParam == VK_F11) return 0;  // our hotkeys
            if (g_capture) {
                g_keysDown.erase((UINT)wParam);
                int id = g_layers.KeyTarget();
                if (id) {
                    UINT vk = (UINT)wParam;
                    UINT sc = (UINT)((lParam >> 16) & 0xff);
                    bool ext = (lParam >> 24) & 1;
                    SendCommand("K" + std::to_string(id) + " 0 " + std::to_string(vk) + " " +
                                std::to_string(sc) + " " + (ext ? "1" : "0") + "\n");
                    return 0;
                }
            }
            break;
        case WM_CHAR:
        case WM_SYSCHAR:
            if (g_capture) {
                int id = g_layers.KeyTarget();
                if (id) {
                    SendCommand("U" + std::to_string(id) + " " + std::to_string((int)wParam) + "\n");
                    return 0;
                }
            }
            break;
        case WM_MOUSEMOVE: {
            int id = g_capture ? g_layers.MouseTarget() : 0;
            if (id)
                SendCommand("M" + std::to_string(id) + " " + std::to_string(Cx(lParam)) + " " +
                            std::to_string(Cy(lParam)) + "\n");
            break;
        }
        case WM_MOUSEWHEEL:
            if (g_capture) {
                int id = g_layers.MouseTarget();
                if (id) {
                    int delta = (short)HIWORD(wParam);
                    POINT p = {Cx(lParam), Cy(lParam)};
                    ScreenToClient(hwnd, &p);
                    SendCommand("W" + std::to_string(id) + " " + std::to_string(delta) + " " +
                                std::to_string(p.x) + " " + std::to_string(p.y) + "\n");
                    return 0;
                }
            }
            break;
        case WM_LBUTTONDOWN: case WM_LBUTTONUP:
        case WM_RBUTTONDOWN: case WM_RBUTTONUP:
        case WM_MBUTTONDOWN: case WM_MBUTTONUP: {
            int id = g_capture ? g_layers.MouseTarget() : 0;
            if (id) {
                bool down = (msg == WM_LBUTTONDOWN || msg == WM_RBUTTONDOWN || msg == WM_MBUTTONDOWN);
                int button = (msg == WM_LBUTTONDOWN || msg == WM_LBUTTONUP) ? 0 :
                             (msg == WM_RBUTTONDOWN || msg == WM_RBUTTONUP) ? 2 : 1;
                SendCommand("B" + std::to_string(id) + " " + std::to_string(button) + " " +
                            (down ? "1" : "0") + " " + std::to_string(Cx(lParam)) + " " +
                            std::to_string(Cy(lParam)) + "\n");
                g_mouseDown[button] = down;
                return 0;
            }
            break;
        }
        case WM_SIZE:
            g_renderer.Invalidate();   // re-create resources on next Present
            break;
        case WM_KILLFOCUS:
            SUI_LOG_INFO("WM_KILLFOCUS (alt-tab).");
            FocusLost();
            break;
        case WM_ACTIVATE:
            if (LOWORD(wParam) == WA_INACTIVE) FocusLost();
            break;
        default:
            break;
    }
    return CallWindowProcW(g_oldWndProc, hwnd, msg, wParam, lParam);
}

HRESULT WINAPI PresentHook(IDXGISwapChain* swapChain, UINT syncInterval, UINT flags) {
    // Lazy resource init + viewport sync from the very first Present, regardless
    // of frame availability (input scaling must exist even before first paint).
    static uint64_t s_nextEnsureAttempt = 0;
    static bool s_compositePausedLogged = false;
    static bool s_wasComposited = false;   // declared above the init block: re-init clears it
    const bool newSwapChain = (g_swapChain != swapChain);
    if (newSwapChain || !g_renderer.ready()) {
        const uint64_t now = GetTickCount64();
        if (newSwapChain || now >= s_nextEnsureAttempt) {
            if (newSwapChain) {
                SUI_LOG_INFO("swapchain changed (%p -> %p); re-initializing resources.",
                             (void*)g_swapChain, (void*)swapChain);
            }
            g_swapChain = swapChain;
            if (g_renderer.EnsureResources(swapChain)) {
                g_viewportSent = false;
                s_nextEnsureAttempt = 0;
                // Textures were (re)created empty: force-upload every layer's
                // resident frame on this Present, otherwise a layer stays
                // invisible until its page happens to repaint (observed as the
                // P2 panel "randomly disappearing then coming back" after GTA
                // recreated the swapchain).
                s_wasComposited = false;
                if (s_compositePausedLogged) {
                    SUI_LOG_INFO("renderer recovered; compositing resumes.");
                    s_compositePausedLogged = false;
                }
            } else {
                // Device lost / mid-recreate: retry at most once a second so a
                // dead device cannot churn or spam the log every frame.
                s_nextEnsureAttempt = now + 1000;
                if (!s_compositePausedLogged) {
                    SUI_LOG_WARN("renderer not ready (device lost/re-init?); compositing paused.");
                    s_compositePausedLogged = true;
                }
            }
        }
    }
    SyncViewport();

    // First composited frame after any hidden period force-uploads every
    // layer's resident frame so the whole stack lands in the same Present.
    if (g_framesReady && g_visible && g_renderer.ready()) {
        if (g_frameBuf.empty()) g_frameBuf.resize(4096 * 2160 * 4u);
        bool force = !s_wasComposited;
        bool anyFresh = false;
        for (const auto& f : g_layers.PumpFrames(g_frameBuf.data(), g_frameBuf.size(), force)) {
            g_renderer.UploadLayer(f.id, g_frameBuf.data(), (int)f.width, (int)f.height);
            anyFresh = true;
        }
        std::vector<int> order = g_layers.DrawOrder();
        if (!order.empty()) g_renderer.DrawLayers(swapChain, order);
        if (force) {
            SUI_LOG_INFO("compositing %u layer(s) (forced upload=%d)", (unsigned)order.size(),
                         anyFresh ? 1 : 0);
        }
        s_wasComposited = true;
    } else {
        if (s_wasComposited) SUI_LOG_INFO("compositing stopped (hidden/unavailable).");
        s_wasComposited = false;
    }

    HRESULT hr = g_origPresent(swapChain, syncInterval, flags);
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
        // Device-lost recovery: drop every device-dependent resource and let the
        // next Present re-init from whatever device/swapchain the game hands us
        // (GTA V recreates both on a driver reset). Force a full layer re-upload
        // so the stack reappears in the same frame rendering comes back.
        SUI_LOG_WARN("device lost (Present hr=0x%08lX); dropping resources, re-init on next frame.",
                     (unsigned long)hr);
        g_renderer.Invalidate();
        g_swapChain = nullptr;
        g_viewportSent = false;
        s_wasComposited = false;
    }
    return hr;
}

HRESULT WINAPI ResizeBuffersHook(IDXGISwapChain* swapChain, UINT count, UINT w, UINT h,
                                 DXGI_FORMAT format, UINT flags) {
    HRESULT hr = g_origResize(swapChain, count, w, h, format, flags);
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
        g_swapChain = nullptr;   // force a full re-init on the next Present
    }
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