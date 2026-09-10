# Changelog

All notable changes. Format loosely follows [Keep a Changelog](https://keepachangelog.com/).

## [Unreleased]

### Added — 2026-09-09 (Issue #2 milestone: layer stack + content scheme)
- **Layer model (host export table v3)** — `SuiCreateLayer` (ids ≥ 2; budget 8, id 1 = boot
  layer), `SuiCloseLayer`, `SuiSetLayerVisible/IsLayerVisible`, `SuiSetLayerZ/GetLayerZ`,
  `SuiSetLayerClickThrough/IsLayerClickThrough`, `SuiReloadLayer`; issue #1 exports
  (`SuiCmdVisible/Focus/ExecuteJs/Navigate`) now honor the layer id.
- **Multi-layer rendering** — per-layer CEF browser + per-layer shared-memory frame mapping
  (`Local\SentinelUI_Frames_L<id>`) helper-side; per-layer D3D11 textures composited in the
  Present hook back-to-front by z (ties: creation order); single shared staging buffer.
- **Input routing per layer** — captured keyboard goes to the focused layer, mouse to the
  topmost visible non-click-through layer; click-through layers composite but never take
  input. IPC commands now carry the target layer id (`C<id> X<id> S<id> H<id> F<id> J<id>
  N<id> K<id> U<id> M<id> B<id> W<id> L<id>`), with legacy no-id forms still parsed.
- **`sentinelui://<plugin>/<path>` content scheme** — custom standard scheme (STANDARD |
  SECURE | CORS_ENABLED, registered in every process), served by the helper from
  plugin-registered local content roots (`SuiRegisterContentRoot` / `Sui.RegisterContentRoot`).
  Local-only (no network), directory traversal refused (URL path segment validation +
  resolved-path containment under the root), CSP-tight HTML responses
  (`script-src 'self'` — pages ship JS/CSS as files), `Cache-Control: no-store` for hot
  reload, MIME by extension. Default root: `sentinelui://sui/` → `<game>\SentinelUI\content`.
  No `file://` remains in the shipped content path (harness selftest keeps file:// for
  its regression baseline only).
- **Managed API 0.2.0** — `Sui.CreateLayer` really creates layers now (native ids,
  null on refusal); `SuiLayer.Z/ClickThrough/IsVisible/Reload`; per-layer event routing —
  events carry `<nativeId>|kind:body` and `SuiLayer.Message` fires only for the owning
  layer (legacy lines still broadcast).
- **Hotkeys** — F11 toggles an in-game acceptance layer (`sentinelui://sui/demo_hud.html`,
  z 10, click-through HUD strip) to verify stacking + click-through without any plugin.
- **Robustness** — queued page JS now executes only after load commit (J-during-load race
  fixed); events unwrapped ASI-side so the managed sink sees clean `<id>|kind:body` lines;
  graceful browser-close pump before `CefShutdown`.
- **In-game boot hardening (post-deploy testing)** — first `sentinelui://` navigation could
  race CEF child-process scheme registration and fail with `ERR_UNKNOWN_URL_SCHEME`:
  the factory is now registered right after `CefInitialize` (and again from
  `OnContextInitialized`), CEF gets a short settle window before the boot navigation, and
  failing `sentinelui://` loads are retried (5×400 ms). Every CEF process appends its
  registration result to `SentinelUI\logs\scheme_registration.log` (process type included)
  for field diagnosis.
- **IPC transport corruption fixed** — both pipe loops re-issued overlapped I/O on a wait
  timeout while the previous operation was still pending, and waiter events were
  manual-reset (stayed signaled, so later waits returned early). That combination could
  interleave/mangle event lines under load (observed: one console event lost 12 leading
  bytes). Reads/connects now keep waiting without re-issuing, events are auto-reset, and
  cancelled writes are retired before their `OVERLAPPED` is reused.
- **New layers adopt the current game viewport** — a layer created after the initial
  viewport sync (e.g. the F11 demo HUD) used to stay at the 1280x720 default and get
  upscaled to the backbuffer (pixelated). `LayerManager` now remembers the last `V`
  viewport and `LayerClient` adopts it before browser creation.
- **F9 is deterministic for multi-layer stacks** — on the hidden→visible edge the host
  force-uploads every layer's resident frame, so the whole stack composites in one
  Present (no per-layer appear-delay); the helper also pauses/resumes all pages
  (`WasHidden`) so a hidden overlay is truly idle and repaints on show.
- **F11 is instant** — first press creates the demo HUD layer, subsequent presses are a
  pure show/hide (no browser re-create; the create took ~250 ms).
- **Orphan helper guard** — if GTA V exits without the managed shutdown path (crash/kill),
  the helper now exits after the pipe has been down for 30 s instead of lingering as a
  CEF process tree that locks the game folder. `pack.ps1` refuses to deploy while GTA5
  runs and no longer deletes `SentinelUI\logs`/`cache` (content only), so a locked cache
  can no longer abort a deploy mid-way.

Field lesson (added to LESSONS-LEARNED §3): `CefResponse::SetMimeType` wants a **bare**
MIME type — `text/html; charset=utf-8` produces an unparseable Content-Type and Chromium
renders the page as a source view instead of HTML.

Bugs fixed along the way: query exports marshalled as `int()` instead of `int(int)`,
double `E<len>:` envelope leak into the managed sink, J-during-load drop.

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