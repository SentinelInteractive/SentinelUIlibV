# P1 acceptance gate — running log

Formal gate from PLAN.md §4 (P1). Status updated 2026-09-09 after the input/UX work (issue #1).

| Check | Status | Notes |
|---|---|---|
| Overlay renders (CEF → shared memory → Present hook) | ✅ | Confirmed in-game, 1914×1046 |
| F9 show/hide | ✅ | Verified repeatedly |
| F10 input capture (game frozen) | ✅ | WASD + mouse look frozen |
| Software cursor tracks mouse | ✅ | Verified (M commands flow) |
| Click-through & page interaction | ✅ | Ping button fires `NUI_TEST:…` |
| Viewport sync to backbuffer | ✅ | `helper viewport -> 1914x1046` |
| Alt-tab / WM_KILLFOCUS | ✅ | Verified in-game 2026-09-10: `capture released on focus loss` + `Capture -> off` on alt-tab while captured (key-ups synthesized) |
| Borderless ⇄ fullscreen / resolution change | ⏳ | ResizeBuffers path implemented; untested in-game |
| Steam overlay / OBS on/off | ⏳ | Untested |
| RAGENativeUI menu (F11) coexistence | ⏳ | Untested — RAGENativeUI is not currently installed in the test install |
| LSPDFR on-duty ⇄ off-duty with layer open | ⏳ | Untested |
| 20× consecutive restarts (exit/leak) | ⏳ | Harness ready (`tools\stress.ps1 -Sessions 20 -AllowNoBootstrap` for lifecycle; `-QuitMode Manual` for clean-shutdown) |
| DXGI device-lost recovery | ⏳ | Handler implemented 2026-09-10 (resources dropped + re-init on next frame); verify via `scenario.ps1 -Scenario DeviceLost` (Win+Ctrl+Shift+B) |
| BSOD-free at shutdown | ✅* | One 0xD1 (driver) occurred mid-work; provenance under review, GPU churn reduced |

Notes (2026-09-10):
- Crash capture (minidumps) landed: `SentinelUI_Host.exe --crash-test` writes a dump; host via `SENTINELUI_CRASH_TEST=1`.
- Launch via `RagePluginHook.exe` (no admin needed — asInvoker manifest). The root ASI is
  auto-loaded by ScriptHookV at process start — this is load-bearing: RPH's hook guard
  reverts foreign Present hooks installed after its own (verified 2026-09-11: Bootstrap
  cold-load attempt crashed with `Hook on our Present hook detected`, AV in `dxgi!Present`).
  Sessions without Bootstrap are overlay-only and failed by the harness unless
  `-AllowNoBootstrap` is passed (headless host: no graceful shutdown).
- **Helper orphan guard verified ×3 in-game**: helper + CEF children exit ~30s after the game dies (pipe-down watchdog).
- **Automation limits discovered**: Steam shows a "Launch Game with custom arguments" dialog on every RPH launch (harness auto-clicks the blue Continue by pixel detection); GTA's own UI ignores synthetic input, so story mode (→ Bootstrap) and the quit confirmation need a human; RPH loads Bootstrap only when the session starts.
- First fully green harness session: 1/1 PASS (`tools\stress-results\stress-20260910_162725.csv`), RSS +8.7 MB over the session, no dumps, no orphans.

Legend: ✅ verified in-game · ⏳ pending matrix pass.