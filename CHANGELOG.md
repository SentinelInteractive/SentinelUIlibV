# Changelog

All notable changes. Format loosely follows [Keep a Changelog](https://keepachangelog.com/).

## [Unreleased]

### Added — 2026-09-09 (Issue #1 milestone, PR #2)
- **Input capture (F10)** — mouse/keyboard route from the ASI to the CEF helper while
  captured; the game freezes its controls (managed control-blocking fiber, all control
  indices).
- **Host ⇄ helper IPC** — named pipe (`\\.\pipe\SentinelUI`) with proper OVERLAPPED I/O on
  both sides: commands (show/hide/focus/exec-js/navigate/viewport/input) and events
  (console/load/page messages).
- **JS → C# bridge** — page `console.log("NUI_…")` travels: helper → pipe → ASI queue →
  `Sui.Pump()` → `Sui.MessageReceived` (+ per-layer events). Round-trip proven by the
  HostProbe (`Sui.ExecuteJs` → page console → event back, assertion of the payload).
- **Software cursor** — pages render a click-through arrow cursor (GTA hides the OS cursor
  and OSR draws none).
- **Viewport sync** — the helper browser resizes to the game backbuffer on first Present
  and after reconnect, so mouse coordinates land where they look.
- **Stability** — D3D11 RTV/SRV views cached (no per-frame object churn); pipe shutdown is
  clean and joinable; pack deploy made failure-safe.

Bugs fixed along the way: F10 toggling twice per press, blocked pipe I/O (the "cursor never
moves" culprit), missing viewport resize, incomplete control-block list, hard references to
paths containing spaces.

## [0.1.0] — 2026-09-08 (initial scaffold + P1 overlay)
- Framework scaffold, CEF pinned (LTS 144), helper-process display pipeline rendering in-game,
  managed API + Bootstrap skeleton, host probes, packaging tooling.