// Game hooks for the SentinelUI host:
//   * IDXGISwapChain::Present / ResizeBuffers — function-level detours via MinHook.
//     Present composites the visible layer stack onto the game backbuffer
//     (game thread, z order, issue #2).
//   * WndProc subclass on the GTA V window — input forwarding + hotkeys.
//     (F9 toggle visibility, F10 toggle input capture, F11 demo HUD layer.)
//     Captured input is routed per layer (focused/topmost interactive).

#pragma once
#ifndef SENTINEL_HOST_HOOKS_H
#define SENTINEL_HOST_HOOKS_H

#include <windows.h>
#include <d3d11.h>

namespace sui_host {

bool InstallHooks(HWND gameWindow);
void RemoveHooks();
// Captured-state side effects (unclip/show cursor + poll mouse position).
void OnCaptureChanged(bool on);
// F11 acceptance path: spawn/close the sentinelui://sui/demo_hud.html layer
// (click-through HUD strip above the boot layer). Defined in host.cpp.
void ToggleDemoLayer();

}  // namespace sui_host

#endif