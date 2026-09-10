# SentinelUI

A **UI library for RagePluginHook (RPH) and LSPDFR** plugins in GTA V single player.
Plugin authors build in-game interfaces from plain **HTML/CSS/JS web components**, rendered by a
dedicated **Chromium (CEF)** process and composited onto the game — callable from LSPDFR callout
plugins and RPH plugins, with **no ScriptHookV / ScriptHookVDotNet dependency**.

```
WEB LAYER      <sui-panel> <sui-button> <sui-list> ... plain HTML/CSS/JS pages
RENDER PROCESS SentinelUI_Host.exe   (dedicated CEF process — transparent OSR → shared memory)
COMPOSITOR     SentinelUI.asi        (hooks D3D11 Present/WndProc; uploads SHM frames; spawns/supervises the helper)
MANAGED API    SentinelUI.API.dll    (net48 static facade + events, exports of the ASI)
BOOTSTRAP      SentinelUI.Bootstrap.dll (RPH plugin: loads host, pump, clean shutdown)
```

## Why the helper-process split?
CEF embedded *inside* a DLL in the game process failfaste with Chromium's hard `0xC0000409` on the
first libcef call (verified by bisecting against the working EXE harness and by decompiling the
WebUI binary — same wrapper, nothing exotic). Running CEF in its **own EXE process** (the shape the
Phase-0 harness proved) sidesteps that entirely, and gives a bonus: **a page/browser crash can never
take the game down.**

## Repository layout

| Path | Contents |
|---|---|
| `docs/` | Research material (LemonUI/RAGENativeUI sources, WebUI example, natives.json) + `PLAN.md` (authoritative plan), `PHASE0.md` (decision record) |
| `native/host/` | **ASI compositor**: MinHook swapchain hooks, WndProc, SHM frame consumer, renderer, `Sui*` exports |
| `native/helper/` | **SentinelUI_Host.exe**: dedicated CEF process, transparent OSR, shared-memory frame publisher, self-test mode |
| `native/hoststub/` | Fake host used by the managed load-path probe |
| `native/harness/` | OSR validation harness (Phase-0 gate, still the regression baseline) |
| `managed/SentinelUI.API/` | Managed bridge (net48), `Sui`/`SuiLayer` facade |
| `managed/SentinelUI.Bootstrap/` | RPH plugin: loads host, per-frame pump, graceful shutdown, AppDomain assembly resolution |
| `managed/tests/HostProbe/` | Console probe: managed⇄host contract (load, handshake, shutdown) |
| `web/content/` | Demo pages (`p1_demo.html`, selftest page) + future `sui-*` toolkit |
| `tools/` | `init-cef.ps1`, `build-{host,helper,harness,hoststub}.ps1`, `pack.ps1` (game deploy) |
| `version.json` | Runtime version manifest |

## Status (2026-09-08)

- ✅ **P0**: CEF pinned (LTS 144, validated); load-path/marshalling & OSR gates PASS.
- ✅ **P1 core**: overlay **confirmed rendering in-game** — Chromium page composited onto GTA V
  (helper process → shared memory → ASI Present hook). F9 show/hide works.
- ⏳ **P1 stability matrix** (resolution/borderless, Steam overlay, RAGENativeUI, LSPDFR cycles,
  20× restarts) — issue #4.
- ✅ **P2**: input capture + JS↔C# bridge **verified in-game** (issue #1). Layer stack +
  `sentinelui://` scheme **verified in-game** (issue #2, 2026-09-10 sessions: two layers,
  z-order, click-through routing, F9/F11 toggles, per-layer viewport) — see `docs/GATE-P2.md`.
- ⏳ **P3**: full managed API + RPH/LSPDFR samples + NuGet + version handshake — issue #3.
- ⏳ **P4**: web component toolkit (`sui-*`, theme, gallery) — issue #5.
- ⏳ **P5**: hardening & release (perf gates, crash capture, coexistence, packaging) — issue #6.

Tracked in GitHub issues: https://github.com/maminka19482/SentinelUIlib/issues

## Current deploy layout (GTA V root)

```
<game>\SentinelUI.asi          ASI compositor (auto-loaded like any ASI)
<game>\SentinelUI_Host.exe     dedicated CEF process
<game>\libcef.dll + runtime    flat at root, next to the ASI
<game>\locales\
<game>\SentinelUI\             content\ · logs\ · cache\ · LICENSE_CEF.txt
<game>\Plugins\                SentinelUI.Bootstrap.dll (+ API.dll)
```

## Target environment (verified)

- GTA V **Legacy** (not Enhanced) — proven on Steam, build 1.0.3889.x, D3D11
- RagePluginHook 1.131.x (SDK on NuGet: 1.124.0), LSPD First Response 0.4.9
- Windows 10/11 x64, .NET Framework 4.8, no SHVDN required

## Build

- Managed: `dotnet build managed\SentinelUI.sln`
- Native: `tools\build-host.ps1` + `tools\build-helper.ps1` (vswhere → vcvars64 → Ninja + MSVC; CEF
  via `tools\init-cef.ps1`, pinned in `native\third_party\cef-current.json`)
- Deploy: `tools\pack.ps1` (copies to the game folder; reversible)

## License

TBD (project license not yet chosen — ask maintainer). CEF is BSD-licensed; attribution ships with
the runtime.