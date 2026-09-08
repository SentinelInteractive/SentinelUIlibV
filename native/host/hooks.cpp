// SentinelUI host (ASI) — game hooks:
//   * IDXGISwapChain::Present / ResizeBuffers — function-level detours via MinHook.
//     Present uploads the latest shared-memory frame (written by the helper
//     process) to the game's D3D11 texture and composites it (game thread).
//   * WndProc subclass on the GTA V window — P1 hotkeys (F9 show/hide).
//     Input capture routing to the helper arrives with the IPC layer (P2).

#include "hooks.h"
#include "framesrc.h"
#include "renderer.h"
#include "suilog.h"

#include <MinHook.h>

#include <d3d11.h>
#include <vector>

#pragma comment(lib, "d3d11.lib")

namespace sui_host {

extern Renderer g_renderer;    // defined in host.cpp
extern FrameSource g_frames;   // defined in host.cpp
extern bool g_visible;         // defined in host.cpp
extern bool g_framesReady;     // defined in host.cpp

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

void ToggleHotkeys(WPARAM wParam, LPARAM lParam) {
    if (wParam != VK_F9 && wParam != VK_F10) return;
    if (lParam & 0x40000000) return;  // auto-repeat
    if (wParam == VK_F9) {
        g_visible = !g_visible;
        SUI_LOG_INFO("F9: visibility -> %s", g_visible ? "visible" : "hidden");
    } else {
        SUI_LOG_INFO("F10: input capture toggle (IPC lands in P2).");
    }
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_KEYDOWN:
        case WM_KEYUP:
        case WM_SYSKEYDOWN:
        case WM_SYSKEYUP:
            ToggleHotkeys(wParam, lParam);
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
    if (!g_framesReady) return g_origPresent(swapChain, syncInterval, flags);

    // Lazy resource init on the first Present (game device may not exist earlier).
    if (!g_renderer.ready() || g_swapChain != swapChain) {
        g_swapChain = swapChain;
        if (!g_renderer.EnsureResources(swapChain)) {
            g_renderer.Invalidate();
        }
    }

    if (g_visible && g_renderer.ready()) {
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
    return hr;
}

}  // namespace

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