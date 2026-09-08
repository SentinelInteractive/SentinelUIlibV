#include "hooks.h"
#include "cef_host.h"
#include "renderer.h"
#include "suilog.h"

#include <MinHook.h>

#include <d3d11.h>

#pragma comment(lib, "d3d11.lib")

namespace sui_host {

extern CefHost g_host;        // defined in host.cpp
extern Renderer g_renderer;   // defined in host.cpp

namespace {

HWND g_gameWindow = nullptr;
WNDPROC g_oldWndProc = nullptr;
bool g_hooked = false;

// ---- swapchain detour targets ----
typedef HRESULT(WINAPI* PresentFn)(IDXGISwapChain*, UINT, UINT);
typedef HRESULT(WINAPI* ResizeFn)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
PresentFn g_origPresent = nullptr;
ResizeFn g_origResize = nullptr;

IDXGISwapChain* g_swapChain = nullptr;

// ---- WndProc helpers ----

bool ShouldSkipRepeat(UINT msg, LPARAM lParam) {
    return msg == WM_KEYDOWN && (lParam & 0x40000000);
}

void ToggleHotkeys(WPARAM wParam, LPARAM lParam) {
    if (wParam != VK_F9 && wParam != VK_F10) return;
    if (lParam & 0x40000000) return;  // repeat
    if (wParam == VK_F9) {
        bool v = !g_host.visible();
        g_host.SetVisible(v);
        SUI_LOG_INFO("F9: visibility -> %s", v ? "visible" : "hidden");
    } else {
        bool c = !g_host.capture();
        g_host.SetCapture(c);
        SUI_LOG_INFO("F10: capture -> %s", c ? "on" : "off");
    }
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_KEYDOWN:
        case WM_KEYUP:
        case WM_SYSKEYDOWN:
        case WM_SYSKEYUP: {
            ToggleHotkeys(wParam, lParam);
            if (g_host.capture() && g_host.visible()) {
                UINT vk = (UINT)wParam;
                UINT sc = (UINT)((lParam >> 16) & 0xff);
                bool ext = (lParam >> 24) & 1;
                bool down = (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN);
                g_host.SendKey(down, vk, sc, ext);
                return 0;
            }
            break;
        }
        case WM_CHAR:
        case WM_SYSCHAR:
            if (g_host.capture() && g_host.visible()) {
                g_host.SendChar((wchar_t)wParam);
                return 0;
            }
            break;
        case WM_MOUSEMOVE: {
            int x = (short)LOWORD(lParam), y = (short)HIWORD(lParam);
            if (g_host.capture() && g_host.visible())
                g_host.SendMouseMove(x, y);
            break;
        }
        case WM_MOUSEWHEEL: {
            int delta = (short)HIWORD(wParam);
            POINT p = {(short)LOWORD(lParam), (short)HIWORD(lParam)};
            ScreenToClient(hwnd, &p);
            if (g_host.capture() && g_host.visible())
                g_host.SendMouseWheel(p.x, p.y, delta);
            return 0;
        }
        case WM_LBUTTONDOWN: case WM_LBUTTONUP:
        case WM_RBUTTONDOWN: case WM_RBUTTONUP:
        case WM_MBUTTONDOWN: case WM_MBUTTONUP: {
            bool down = (msg == WM_LBUTTONDOWN || msg == WM_RBUTTONDOWN || msg == WM_MBUTTONDOWN);
            int button = (msg == WM_LBUTTONDOWN || msg == WM_LBUTTONUP) ? 0 :
                         (msg == WM_RBUTTONDOWN || msg == WM_RBUTTONUP) ? 2 : 1;
            int x = (short)LOWORD(lParam), y = (short)HIWORD(lParam);
            if (g_host.capture() && g_host.visible()) {
                g_host.SendMouseButton(x, y, button, down, (UINT)wParam);
                return 0;
            }
            break;
        }
        case WM_SIZE:
            if (g_host.started()) {
                g_host.ResizeView(LOWORD(lParam), HIWORD(lParam));
                g_renderer.Invalidate();
            }
            break;
        case WM_KILLFOCUS:
            // Focus loss: CEF keeps keys down unless we synth key-ups. P1: log only.
            SUI_LOG_INFO("WM_KILLFOCUS (alt-tab).");
            break;
        default:
            break;
    }
    return CallWindowProcW(g_oldWndProc, hwnd, msg, wParam, lParam);
}

// ---- swapchain hooks ----

HRESULT WINAPI PresentHook(IDXGISwapChain* swapChain, UINT syncInterval, UINT flags) {
    if (!g_host.started()) return g_origPresent(swapChain, syncInterval, flags);

    // Lazy resource init on the first Present (game device may not exist earlier).
    if (!g_renderer.ready() || g_swapChain != swapChain) {
        g_swapChain = swapChain;
        if (!g_renderer.EnsureResources(swapChain)) {
            SUI_LOG_WARN("ENSURE_RESOURCES_FAILED");
        }
    }

    if (g_host.visible() && g_renderer.ready()) {
        Frame frame;
        if (g_host.TakeFrame(frame) && !frame.bgra.empty()) {
            g_renderer.UploadFrame(frame.bgra, frame.width, frame.height);
        }
        g_renderer.DrawFrame(swapChain);   // redraw persists the last texture
    }

    return g_origPresent(swapChain, syncInterval, flags);
}

HRESULT WINAPI ResizeBuffersHook(IDXGISwapChain* swapChain, UINT count, UINT w, UINT h,
                                 DXGI_FORMAT format, UINT flags) {
    HRESULT hr = g_origResize(swapChain, count, w, h, format, flags);
    g_renderer.Invalidate();
    if (g_host.started()) g_host.ResizeView((int)w, (int)h);
    SUI_LOG_INFO("ResizeBuffers -> %ux%u", w, h);
    return hr;
}

}  // namespace

bool InstallHooks(HWND gameWindow) {
    if (g_hooked) return true;
    g_gameWindow = gameWindow;

    // 1. WndProc subclass.
    g_oldWndProc = (WNDPROC)SetWindowLongPtrW(g_gameWindow, GWLP_WNDPROC, (LONG_PTR)&WindowProc);
    if (!g_oldWndProc) {
        SUI_LOG_ERROR("SetWindowLongPtrW failed (%lu).", GetLastError());
        return false;
    }

    // 2. Resolve Present/ResizeBuffers addresses via a dummy swapchain.
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
        SUI_LOG_ERROR("Dummy swapchain creation failed; hooks aborted.");
        if (dummy) DestroyWindow(dummy);
        return false;
    }

    void** vtable = *(void***)chain;
    PresentFn presentAddr = (PresentFn)vtable[8];
    ResizeFn resizeAddr = (ResizeFn)vtable[13];

    if (dev) dev->Release();
    if (chain) chain->Release();
    if (dummy) DestroyWindow(dummy);

    // 3. Function-level detours via MinHook.
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