# SentinelUI

A web-component UI library for **RagePluginHook (RPH) and LSPDFR** plugins in GTA V single player.
Plugin authors build in-game interfaces from plain **HTML/CSS/JS web components**, rendered by an
embedded Chromium (**CEF**) overlay — callable from LSPDFR callout plugins and RPH plugins, with
**no dependency on ScriptHookV or ScriptHookVDotNet**.

```
WEB LAYER      <sui-panel> <sui-list> <sui-button> ... plain HTML/CSS/JS (sentinelui:// scheme)
NATIVE HOST    SentinelUI.asi  (C++20 + CEF)  hooks D3D11 Present, renders layer stack
MANAGED API    SentinelUI.API.dll  (net472, static API + events, DllImport of host exports)
BOOTSTRAP      SentinelUI.Bootstrap.dll (RPH plugin, owns host lifecycle + pump)
```

## Repository layout

| Path | Contents |
|---|---|
| `docs/` | Research material (LemonUI / RAGENativeUI sources, WebUI example [binaries kept local-only], natives.json native DB, `PLAN.md` = authoritative plan) |
| `native/` | CEF host (`SentinelUI.asi`) + worker subprocess (Phase 1+) |
| `managed/SentinelUI.API/` | Managed bridge library, net472 x64 |
| `managed/SentinelUI.Bootstrap/` | RPH plugin lifecycle owner |
| `managed/samples/` | RPH plugin sample + LSPDFR callout sample (Phase 3) |
| `web/` | Web component toolkit (`sui-*` elements, theme, docs gallery) |
| `tools/` | Packaging / CEF download scripts, codegen from natives.json |
| `version.json` | Runtime version manifest + compatibility handshake data |

## Status

**Phase 0 — Decisions & spikes** (CEF version pin, load-path probe, LSPDFR domain probe, hook coexistence).
See `docs/PLAN.md` for phases, gates, acceptance matrix and the risk register.

## Target environment (verified)

- GTA V **Legacy** (not Enhanced) — Steam, build 1.0.3889.x, D3D11
- RagePluginHook 1.131.x (SDK on NuGet: 1.124.0), LSPD First Response 0.4.9
- Windows 10/11 x64, .NET Framework 4.7.2, no SHVDN required

## License

TBD (project license not yet chosen — ask maintainer).
CEF is BSD-licensed; attribution files ship with the runtime layout.
