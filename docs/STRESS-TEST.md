# SentinelUI stress runbook (issue #4 — P1/P2 acceptance matrix)

How to run the stability gate on the real install, unattended. Two tools:

| Tool | Purpose |
|---|---|
| `tools\stress.ps1` | N full game sessions: launch → wait for host → scenario → sample RSS/handles → quit → orphan/dump/log checks. Writes CSV + JSON results. |
| `tools\scenario.ps1` | One in-session scenario against a running game (`-Scenario QuitGame` also unsticks a stuck session). |
| `tools\scenario-lib.ps1` | Shared implementation (dot-sourced; no param block on purpose). |

## Preconditions

1. **Normal (non-elevated) terminal is fine** — `RagePluginHook.exe` is `asInvoker`; no UAC prompt is expected.
   If a previous session ended uncleanly, **restart Steam and/or the Rockstar Games Launcher first**:
   a terminated-but-unreaped `GTA5` entry makes them refuse to start a new instance (the preflight warns
   about this and liveness checks ignore zombie entries).
2. Game closed (a terminating zombie entry is ignored; a live `GTA5` process aborts preflight).
3. No leftover `SentinelUI_Host` processes. Use `-CleanupOrphans`, or:
   `Get-Process SentinelUI_Host | Stop-Process -Force`
4. Fresh deploy: `tools\pack.ps1` (refuses while GTA5 runs; preserves logs/cache).
5. Steam running if you use `-Launch Steam`.

## Launch modes (`-Launch`)

| Mode | Command | Notes |
|---|---|---|
| `RPH` (default) | `RagePluginHook.exe` | **Required for a valid session.** RPH loads `SentinelUI.Bootstrap.dll`, which owns graceful shutdown and event pump. |
| `Steam` | `steam://rungameid/271590` | Only if your install injects RPH via proxy. Without RPH there is no Bootstrap. |
| `Direct` | `GTA5.exe` | Same caveat. |

> **Why this matters:** ScriptHookV (`dinput8.dll` + `ScriptHookV.dll`) auto-loads any `.asi`
> in the game root. That is *required* for hook order: SentinelUI's Present hook must exist
> before RPH installs its own hook — RPH's guard reverts foreign Present hooks installed
> after its own and crashes the D3D chain (verified 2026-09-11; see `docs/LESSONS-LEARNED.md`).
> Launching without RPH still starts `SentinelUI.asi` — a **headless host** with no managed
> shutdown path — so the harness requires `[SentinelUI] Bootstrap ready` unless
> `-AllowNoBootstrap` opts into overlay-only checks.

## Automated loop

```powershell
# 20 sessions, demo HUD open for each 45s hold (leak/soak baseline)
pwsh -File tools\stress.ps1 -Sessions 20 -Scenario HudTicks -HoldSeconds 45

# quick smoke: one session, capture + alt-tab scenario
pwsh -File tools\stress.ps1 -Sessions 1 -Scenario CaptureAltTab -HoldSeconds 20

# see the plan without launching anything
pwsh -File tools\stress.ps1 -DryRun
```

Useful switches: `-GameDir`, `-Launch RPH|Steam|Direct`, `-ReadyTimeoutSeconds 240`,
`-ExitTimeoutSeconds 45`, `-OrphanTimeoutSeconds 45`, `-SampleSeconds 10`,
`-MaxConsecutiveFailures 3`, `-QuitKey Enter|Y|Space`, `-ForceKillOnTimeout`,
`-CleanupOrphans`.

### Quit modes (`-QuitMode`)

GTA V's quit confirmation ("beenden", Ja/Nein) **cannot be confirmed by automation**: the
game's own UI ignores synthetic input (DirectInput/RawInput), even though SentinelUI hotkeys
work (our ASI reads the WndProc messages directly). Verified 2026-09-10: SendInput
keyboard/mouse, posted `WM_KEY*`, `WM_ACTIVATE(APP)`, `WM_SYSCOMMAND/SC_CLOSE` and
`WM_QUERYENDSESSION`/`WM_ENDSESSION` were all ignored while F9 still reached the host.

| Mode | Behavior | Use for |
|---|---|---|
| `Kill` (default) | `Stop-Process -Force`, then verifies the helper's 30s pipe-down guard cleaned up helper + CEF children | unattended runs (20× restarts) |
| `Manual` | waits up to `-ManualQuitTimeoutSeconds` for you to close the game (Alt+F4 + confirm) | verifying clean shutdown (`Bootstrap unloaded`) |
| `Confirm` | best-effort Alt+F4 + `-QuitKey` repeats | only if your setup accepts injected input |

### Pass criteria (per session)

- `ReadySeconds` recorded, and `[SentinelUI] Bootstrap ready` in the RPH log.
- `Orphans = 0` after the orphan window (helper + CEF children; 30s pipe-down guard).
- `DumpsAdded = 0`.
- Quit: `Kill` mode is accepted on orphan cleanup alone; `Manual`/`Confirm` additionally
  require `CleanExit` + `Bootstrap unloaded`.

Results: `tools\stress-results\stress-<timestamp>.csv|json`. Read the CSV columns
`GtaRssStartMB`/`GtaRssEndMB` for leak drift and `Notes` for per-session detail.

## In-session scenarios

```powershell
pwsh -File tools\scenario.ps1 -Scenario CaptureAltTab
pwsh -File tools\scenario.ps1 -Scenario DeviceLost
pwsh -File tools\scenario.ps1 -Scenario QuitGame      # unstick a running game
```

| Scenario | Steps | Evidence to check in `SentinelUI\logs\sentinelui_host_*.log` |
|---|---|---|
| `LayerChurn` | F11 + F9 churn (5 cycles) | `compositing N layer(s)` ↔ `compositing stopped` pairs, no stalls |
| `CaptureAltTab` | F10 on → Alt+Tab away/back | `Capture -> on`, `WM_KILLFOCUS`, `capture released on focus loss`, `Capture -> off` |
| `Resize` | Alt+Enter round-trip (or resolution change in settings) | `viewport sync -> WxH`, no blank layer afterwards |
| `DeviceLost` | Win+Ctrl+Shift+B GPU driver reset | `device lost (Present hr=…)` followed by a new `Renderer ready` / `layer N texture` |
| `GameBar` | Win+Alt+R start/stop recording | no stalls, frames continue |
| `HudTicks` | F11 demo HUD left open | `NUI_HUD:hud-tick` keeps arriving; RSS stable |
| `QuitGame` | Alt+F4 + confirm keys | `Shutdown requested`, `Helper process stopped`, `Hooks removed`, `Host stopped` |

All steps are also appended to `<game>\SentinelUI\logs\stress_markers.log` with
timestamps so scenarios can be correlated with host/RPH logs.

## Manual checklist (cannot be safely automated)

Record each result in `docs/GATE-P1.md`.

| Check | How | Pass looks like |
|---|---|---|
| Steam overlay ON/OFF | Steam settings toggle; Shift+Tab while layers open | overlay opens/closes, no stall or dead input |
| OBS / Game Bar capture | OBS game capture or Win+Alt+R during layer interaction | no dropped-frame spike, overlay intact |
| RAGENativeUI (F11) | install it, open its menu while a layer is visible | HTML below RPH-drawn content per plan; no input theft (note: not currently installed) |
| LSPDFR on-duty ⇄ off-duty | force duty / go off duty with a layer open | no domain-unload crash, layer survives/cleans up |
| Alt-tab / WM_KILLFOCUS | Alt+Tab away and back while captured | capture released (auto), cursor restored, no stuck input |
| 4K + 150% scaling, multi-monitor | change desktop scale / drag across monitors | cursor maps correctly, no DPI blur |
| Monitor sleep / workstation lock | Win+L, monitor power save | overlay recovers on wake |
| Hidden overlay ≈ zero cost | `PresentMon --process_name GTA5.exe` hidden vs visible | hidden ≈ baseline; visible static page ≤3 FPS loss |
| 30-min RSS soak | `-Sessions 1 -Scenario HudTicks -HoldSeconds 1800` | GTA+helper+CEF RSS delta within budget, no monotonic growth |

## Evidence rules

- **Archive logs per session** (`SentinelUI\logs\*.log` + `RagePluginHook.log`) before
  a redeploy overwrites them; the 09-09 issue-1 evidence log was lost this way.
- Minidumps (if any) land in `SentinelUI\logs\*.dmp`. Crash capture can be
  verified without a real crash:
  - helper: `SentinelUI_Host.exe --crash-test` (writes a dump, exits)
  - host: set `SENTINELUI_CRASH_TEST=1` before launching the game (then remove it)
- Never delete `SentinelUI\logs`/`cache` while GTA5 runs; `pack.ps1` already refuses.

## Known limitations

- **GTA V's own UI ignores injected input** — story-mode selection and the quit
  confirmation need a human unless your install auto-loads story mode. Overlay scenarios
  (`HudTicks`, `LayerChurn`, `CaptureAltTab`, `Resize`, `DeviceLost`, `GameBar`) still run
  unattended because SentinelUI handles the hotkeys itself.
- Every `Kill`-mode run exercises the helper orphan guard (observed 2026-09-10: helper +
  5 CEF children exit ~30–35s after the game dies).
- `DeviceLost` resets the GPU driver — close other GPU-heavy apps first.
- `Resize` uses Alt+Enter; if your install ignores it, change resolution in the
  in-game settings instead.
- The harness sends keys only to a foreground GTA window; if the window cannot be
  foregrounded the step is skipped rather than injected elsewhere.
- Steam overlay toggling is a Steam setting and stays manual.
