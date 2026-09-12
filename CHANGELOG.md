# Changelog

All notable changes. Format loosely follows [Keep a Changelog](https://keepachangelog.com/).

## [Unreleased]

### Fixed — 2026-09-12 (custom scheme content loading)
- Scheme CSP now sends `connect-src 'self'` (was `'none'`), so pages served through
  `sentinelui://` are at least allowed to attempt same-origin requests.
- Documented the real limitation found while porting PoliceTerminalPlus: **Chromium does not
  support Fetch/XHR for custom schemes** (`URL scheme "sentinelui" is not supported`). Pages
  must load dynamic content with `<script src>`, `<iframe>` or a bundled fragment script; see
  `docs/PLUGIN-AUTHORING.md` and `docs/LESSONS-LEARNED.md` section 3.

### Added — 2026-09-12 (bridge v2: request/response, `window.sui`, key modifiers)
- **`window.sui` protocol bridge injected into every page** — `sui.send(type, payload)`,
  `sui.on(type, handler)` and `sui.request(type, payload[, timeoutMs])` (Promise). JS→C#
  travels as a versioned JSON envelope (`{v,kind,type,id,payload}`) over the existing console
  event channel; C#→JS is delivered through the injected helper. No native ABI change.
- **Managed request API** — `SuiLayer.Request` / `Sui.RequestReceived` with
  `SuiRequest.Reply` / `ReplyError`; delivered from `Sui.Pump()` on the game thread, so
  handlers may call game natives. `SuiLayer.Send` now posts typed events through the bridge
  (`Send(payload)` still works, type defaults to "message").
- **Key modifiers forwarded** — the ASI reads Shift/Ctrl/Alt while processing the key message
  and appends them to `K` commands; the helper maps them to CEF event flags, so Ctrl+C/V,
  Shift+arrows/selection and Alt combos work in pages. Optional 6th `K` field keeps older
  helpers compatible (no modifiers).
- **Capture convenience** — `SuiLayer.ShowCaptured()` / `HideCaptured()` (show + focus +
  capture / release + hide).
- **DX** — `docs/PLUGIN-AUTHORING.md` quickstart; `samples/browser-dev/` (browser shim + demo
  page to design without the game); HostProbe round-trip tests for owner and cross-AppDomain
  request/reply.
- API assemblies bumped to **0.4.0** (host export table stays v4; no runtime layout change).

### Fixed — 2026-09-11 (panel blink after GTA swapchain re-creation)
- **Layers could stay invisible after GTA recreated its swapchain** (alt-tab, fullscreen /
  display transitions, some menus): resource init re-created the layer textures empty, but the
  compositor only uploaded layers that published a *new* frame — a static page could stay blank
  until its next repaint (the P2 panel "randomly disappearing then coming back"). Resource
  (re)init now forces a re-upload of every layer's resident frame in the same Present, so the
  stack reappears immediately. New `swapchain changed` / `compositing paused|resumed` log lines
  make field diagnosis possible.
- Sample callout probability High → Low (it spawned constantly during testing; `ForceCallout`
  still works on demand).

### Changed — 2026-09-11 (input polish: cursor, capture badge, hotkeys)
- **Software cursor rebuilt** — a white/black arrow with a drop shadow instead of the old
  `mix-blend-mode: difference` shape (which inverted whatever was under it), moved via
  `translate3d`, presses scale the tip. It is now **only visible while input capture is on**:
  previously it froze on screen at the last mouse position after capture dropped (alt-tab),
  which made the UI look glitched.
- **Capture badge** — the same injected script draws a small top-right
  "INPUT CAPTURE • F10 RELEASE" badge while capture is active, so it is obvious when clicks
  will reach the page.
- **Capture state now actually reaches the helper** — bare `C1`/`C0` from `SetCaptureState`
  was silently ignored because `C` is also the create-layer command; the helper parses the
  bare two-byte form as the capture toggle (badge + cursor visibility).
- **F9/F10/F11 are swallowed** on keydown *and* keyup and never forwarded to pages or the game
  (previously pressing F10 while capturing also sent F10 into the page).

### Fixed — 2026-09-11 (mouse clicks were inverted; capture-loss button release)
- **CEF `SendMouseClickEvent`'s third parameter is `mouseUp`, not `mouseDown`.** The helper
  passed the ASI's `down` flag straight through, so every click sent up-before-down; Chromium
  then completed a button press only on every second click (field report: "buttons are buggy,
  sometimes they work"). Fixed: pass `!down` (`helper_app.cpp`).
- **Synthesized button-ups on capture end** (F10 off, alt-tab/focus loss): the ASI tracks
  forwarded mouse buttons and releases any still-held ones, so CEF cannot stay stuck in a
  pressed state that swallows later clicks (`hooks.cpp`).

### Added — 2026-09-11 (issue #3: API 0.3 — cross-domain clients, inline HTML, handshake)
- **Per-AppDomain event queues (host export table v4)** — `SuiRegisterEventClient` /
  `SuiUnregisterEventClient` / `SuiPumpEventClient`: every inbound event is copied to each
  registered domain's bounded queue (oldest dropped + logged) and delivered only on that
  domain's `Pump()` thread — no cross-domain delegate calls. Legacy
  `SuiRegisterEventSink`/`SuiPumpEvents` still work for v3 hosts/API assemblies.
- **Owner vs client model** — only Bootstrap (or an offline probe) owns the host: attaches,
  pumps, calls `SuiShutdown`. Plugin AppDomains attach lazily as clients (`Sui.AttachClient`,
  or first API use): never `LoadLibrary`, never shutdown; their `DomainUnload` closes their
  layers and unregisters their queue. Fixes "one plugin unload kills the UI for everyone" and
  "last Initializer steals all events".
- **`Sui.CreateLayerFromHtml(html, baseUrl)`** — in-memory pages via a `data:text/html` URL,
  with an optional injected `<base href>` for relative subresources; scheme responses now send
  `Access-Control-Allow-Origin: *` so data:-URL layers can load `sentinelui://` assets.
- **Version handshake** — API 0.3.0 refuses hosts older than export table v3 with a clear
  error and warns when a v3 host lacks client queues; `version.json` bumped to `hostExports 4`.
  Duplicate host modules report API `-1` (per-process single-instance mutex).
- **HostProbe** — new inline-HTML round-trip and a real cross-AppDomain client test (LSPDFR
  domain shape): the client domain creates a layer, pumps its own queue and receives its event
  while the owner keeps pumping. Per-layer routing no longer broadcasts events for unknown
  native layer ids (those belong to other domains).
- **P4 web toolkit removed from the plan** (issue #5 closed): SentinelUI stays a generic
  renderer; component libraries are userland.

### Added / Fixed — 2026-09-11 (safe load path + fast orphan cleanup)
- **Bootstrap is attach-only** — `Sui.Initialize(..., loadIfNeeded:false)` attaches to the
  ASI-loader-hosted module via `GetModuleHandle("SentinelUI.asi")` and never `LoadLibrary`s it
  in-game (RPH's Present-hook guard reverts late hooks — 2026-09-11 crash, see LESSONS §2).
  Missing ASI loader => clear log + no-op, no crash. `FreeLibrary` only for modules the API
  itself loaded (offline probes). HostProbe gained an attach-only degradation check.
- **Single-instance guard** — the ASI takes `Local\SentinelUI_Host_Instance_v1` in `DllMain`;
  a stray second module copy (root + subfolder) stays inert and reports API version -1.
- **Helper fast-exit** — the host passes `--parent-pid=<game pid>`; the helper waits on the game
  process and shuts down immediately when it dies (kill/crash), instead of the 30 s pipe-down
  guard (kept as fallback for hosts without the argument).
- **Docs acknowledge the ScriptHookV loader requirement** (README + PLAN §2/§3) and the public,
  obfuscated-runtime distribution model; README no longer claims a "no ScriptHookV dependency".

### Fixed / Changed — 2026-09-11 (root ASI load order is load-bearing; subfolder experiment reverted)
- **Tried relocating the runtime into `<game>\SentinelUI\` with Bootstrap cold-loading the ASI**
  (to remove the ScriptHookV headless-host mode). It crashes instantly in-game: RPH's hook
  guard detects the foreign Present hook installed *after* its own and reverts it
  (`Hook on our Present hook detected / Restoring untouched D3D11Present`), breaking the D3D
  chain — own minidump + RPH crashreport show the AV in `dxgi!Present` at the reverted
  address. Reverted: `SentinelUI.asi` stays in the game root (ScriptHookV loads it at process
  start, before RPH hooks Present) and Bootstrap attaches to the already-loaded module.
  Constraint documented in README / STRESS-TEST / GATE-P1 and `native/host/host.cpp`.
  A subfolder layout requires replacing the Present detour with an RPH-native render path
  (`Game.RawFrameRender`/Graphics) — follow-up work.
- Managed host-path fallbacks (`Bootstrap.ResolveHostPath`, `Sui.Initialize`) and
  `stress.ps1` preflight restored to the root locations; no behavior change otherwise.

### Fixed / Added — 2026-09-10 (stress tooling + hardening for issue #4)
- **Capture auto-releases on focus loss** — `WM_KILLFOCUS`/`WM_ACTIVATE` while
  captured now synthesize key-ups for every forwarded key, release capture and
  restore the cursor (`hooks.cpp`). Previously F10 could stay latched through an
  alt-tab and the page could think a key was still held.
- **DXGI device-lost recovery** — `Present`/`ResizeBuffers` failures with
  `DXGI_ERROR_DEVICE_REMOVED`/`RESET` drop all device resources and re-init on the
  next frame from the game's (re)created device/swapchain; failure retries are
  rate-limited to 1/s so a dead device cannot churn or spam the log.
- **Minidump capture** — host and helper install an unhandled-exception filter
  (`native/host/crash.{h,cpp}`) that writes `SentinelUI\logs\<prefix>_<ts>_pid<PID>.dmp`
  and chains to the previous filter. Fixed buffers only (safe on a corrupted heap).
  Test hooks: helper `--crash-test`, host `SENTINELUI_CRASH_TEST=1`.
- **Stress harness** — `tools\stress.ps1` (unattended N-session loop: RPH launch,
  ready/bootstrap wait, scenario, RSS/handle sampling, quit modes, orphan/dump/log
  checks, CSV+JSON results) plus `tools\scenario-lib.ps1` / `tools\scenario.ps1`
  (LayerChurn, CaptureAltTab, Resize, DeviceLost, GameBar, HudTicks, QuitGame).
  See `docs/STRESS-TEST.md`. Verified 2026-09-10: first green session, focus-loss
  capture release in-game, helper orphan guard ×3 (~30s), Steam launch-dialog
  auto-approval, force-foreground for background operation.
- **Field lesson** — ScriptHookV auto-loads `SentinelUI.asi` in the game root even
  without RPH, producing a headless host with no graceful shutdown; the harness
  therefore launches `RagePluginHook.exe` by default and fails sessions where
  Bootstrap never loaded (unless `-AllowNoBootstrap`). GTA's own UI ignores
  synthetic input, so story-mode start and quit confirmation are manual (`-QuitMode
  Kill` for unattended, `-QuitMode Manual` for clean-shutdown checks).

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