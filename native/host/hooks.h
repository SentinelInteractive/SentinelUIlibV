// Game hooks for the SentinelUI host:
//   * IDXGISwapChain::Present / ResizeBuffers — function-level detours via MinHook.
//     Present draws the latest CEF frame onto the game backbuffer (game thread).
//   * WndProc subclass on the GTA V window — input forwarding + P1 hotkeys.
//     (F9 toggle visibility, F10 toggle input capture.)

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

}  // namespace sui_host

#endif