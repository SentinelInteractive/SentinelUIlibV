# SentinelUI — Authoritative Plan

Status: **P1 core + P2 verified in-game (2026-09-10)**; load-path constraint recorded 2026-09-11
(see decision 2). Source of truth for architecture decisions, gates and acceptance.
Last updated: 2026-09-11.

---

## 1. Product

UI library for GTA V single player (RPH + LSPDFR) letting plugins render **HTML/CSS/JS web components**
in-game via a CEF overlay. Callable from LSPDFR **and** RPH .NET plugins. No ScriptHookV / SHVDN dependency.

## 2. Architecture

Four parts, one process (the game):

```
WEB LAYER       Plain HTML/CSS/JS web components, served from sentinelui://plugin/<file> (custom scheme,
                registered content roots, CSP, hot reload). JS<->host via CefMessageRouter (queries)
                + postMessage; versioned JSON envelope {v,id,layer,type,payload}.
NATIVE HOST     SentinelUI.asi (C++20, pinned CEF). Own dedicated CEF thread + message pump.
                IDXGISwapChain::Present/ResizeBuffers vtable hook (installed AFTER RPH's => HTML renders
                above game output, BELOW RPH Graphics/RawFrameRender content - documented stacking).
                WndProc chain (keys/mouse/IME/focus), virtual cursor module, transparent OSR -> staging
                buffer -> D3D11 texture -> gamma-correct quad in Present. Layer stack with
                z-order/visibility/click-through/focus. C exports + per-token SPSC event queues.
                Minidump handler + structured logs.
MANAGED API     SentinelUI.API.dll (net48 x64, no SHVDN). Static facade; LoadLibrary only for offline
                probes (in-game: attaches to the ASI-loader-hosted module). Owner vs client domains:
                each AppDomain registers a per-domain event queue (host API v4, SuiRegisterEventClient)
                and pumps its own queue on the calling thread (game-thread contract => handlers may
                call natives). Client DomainUnload closes its layers + unregisters its queue; only the
                owner (Bootstrap) calls SuiShutdown. Version handshake refuses hosts < v3.
BOOTSTRAP       SentinelUI.Bootstrap.dll (RPH plugin, <game>\Plugins\). Attach-only host owner:
                GetModuleHandle("SentinelUI.asi") — NEVER LoadLibrary in-game (RPH's hook guard
                reverts Present hooks installed after its own; the ASI loader must load the root
                ASI at process start). Per-frame pump fiber.
                Console commands (sui ...). Graceful shutdown on unload - NEVER in PROCESS_DETACH.
```

- **Canonical load path**: ScriptHookV (ASI loader) loads `<game>\SentinelUI.asi` from the game root
  at process start => Present hook precedes RPH's guard; Bootstrap attaches to that module and owns
  lifecycle. A single-instance mutex makes a stray second module copy inert (API reports -1).
  Without an ASI loader the host never loads: Bootstrap logs it and the UI is a no-op (no crash).
  Field evidence for the constraint: `docs/LESSONS-LEARNED.md` §2 (2026-09-11).
- **Cross-domain**: no managed singletons across AppDomains (LSPDFR plugins vs Bootstrap). Only the native
  host state is shared (same pattern as RAGENativeUI's rnui_shared_state mmf).
- **Stacking guarantee** (tested): HTML above game HUD/pause menu; below RPH-drawn graphics content.
- **SHVDN custom natives**: optional 2-day spike, NOT in critical path.

## 3. Key decisions (post-critique)

1. Host: C++20, CEF pinned at Phase-0 gate result (LTS 144 first candidate), own thin D3D11 hook.
2. Load: ASI loader (ScriptHookV) loads root `SentinelUI.asi` at process start; Bootstrap attaches
   (`GetModuleHandle`, never `LoadLibrary`) and owns lifecycle. Single-instance mutex. See §2 and
   the 2026-09-11 field note (RPH hook guard reverts late Present hooks).
3. Render: transparent OSR, dirty-rect partial uploads, per-layer FPS caps, redraw-on-invalidate,
   WasHidden + timer throttle for hidden layers. sRGB/gamma-correct quad shader.
4. Input: WndProc forwarding; capture-owning GameFiber (DISABLE_CONTROL_ACTION set) while a
   capture-focused layer is visible (managed side owns control blocking); virtual cursor + host cursor;
   focus-loss => synthesized key-ups + WasHidden; IME workstream (Phase 4 stretch).
5. Managed API: static facade; per-AppDomain event tokens + bounded queues (host v4); Pump() drains the
   calling domain's queue on the game thread; client domains never own shutdown; DomainUnload
   auto-unregisters and closes the domain's layers; plugin authors bundle the API DLL next to their plugin.
6. Cross-domain: native-host-only shared state.
7. Content: sentinelui:// scheme, registered roots, local-only by default, CSP, hot reload.
8. Protocol: versioned JSON envelope; JS->host = CefMessageRouter; host->JS = postMessage;
   managed<->host = typed SPSC queues w/ caps + SuiPumpEvents.
9. Web toolkit: **NOT PLANNED** (2026-09-11) — SentinelUI stays a generic HTML/CSS/JS renderer;
   component libraries live in userland. Issue #5 closed.
10. Testing: real install D:\SteamLibrary\...\Grand Theft Auto V (RPH 1.131 + LSPDFR 0.4.9).
11. Licensing: own code; CEF BSD notices shipped; WebUI archive = behavioral spec ONLY (no binaries).
12. Out of scope: GTA V Enhanced (D3D12), FiveM/RageMP, Online (RPH disables), SHVDN-only setups,
    WebGL/GPU-process path (unvalidated).

## 4. Phases & gates

- **P0 Decisions & spikes**: CEF version pin (OSR/Alloy/threading validation harness; decide 144 vs 151),
  RPH-plugin LoadLibrary-of-.asi probe, LSPDFR 0.4.9 domain-model probe (two callouts shipping different
  versions of a shared DLL), swapchain coexistence probe (RPH + Steam overlay + OBS), optional SHVDN
  native call probe. GATE: decision record + repo scaffold building.
- **P1 In-game host gate**: host loaded by Bootstrap in the real install; CEF init on dedicated thread;
  ONE static layer + toggle key + capture toggle. GATE: 20 consecutive sessions, alt-tab, borderless/
  fullscreen switch, Steam overlay on/off, OBS on/off, no FPS loss when hidden, RAGENativeUI menu
  coexistence, LSPDFR on-duty<->off-duty with layer open, no crash.
- **P2 Full host**: complete layer API + exports, z-order/click-through/focus, virtual cursor,
  game-control disable hooks, IME plumbing, sentinelui:// scheme + CSP, message router, dirty-rect
  rendering, ResizeBuffers/device-lost recovery, minidump + structured logging, version handshake.
- **P3 Managed API + samples**: API assembly complete, Bootstrap console commands, RPH sample,
  LSPDFR callout sample (case screen with live game data via natives from pump thread), packaging,
  NuGet (API DLL only; **deferred — publishing step, not needed internally**). In progress
  2026-09-11: cross-domain clients, inline HTML, version handshake landed (API 0.3/host v4).
  2026-09-12: LSPDFR callout sample in-repo (`samples/SuiSample/`); **bridge v2** (`window.sui`
  request/response, key modifiers, `ShowCaptured`) + `docs/PLUGIN-AUTHORING.md` + browser-dev
  shim landed (API 0.4). Remaining: Bootstrap console commands, RPH sample, optional auto-pump.
- **P4 Web toolkit**: NOT PLANNED — removed 2026-09-11 (issue #5 closed).
- **P5 Hardening & release**: 30-min RSS test (<400 MB delta), layer budget (<=3 visible), crash-pack
  triage runbook, version.json matrix (RPH 1.124->1.131+), CEF notices, release zip, wiki.

## 5. Acceptance matrix (must all pass at P1/P5)

Steam overlay ON/OFF; OBS/Game Bar capture; DXGI device-removed recovery; fullscreen-exclusive vs
borderless; 4K + 150% scaling; multi-monitor cursor/DPI; LSPDFR on-duty<->off-duty cycles; RPH plugin
reload; 20x restarts (exit crash/leak); 30-min RSS growth; layer hidden => ~zero cost; static fullscreen
page => <=3 FPS loss; HTML above game pause menu.

## 6. Risks (top)

R1 swapchain-hook conflicts (RPH/Steam/OBS); R2 CEF OSR/thread churn (M128/M130+) => version pin gate;
R3 exit-crash ordering => Bootstrap-owned shutdown + 20x restart gate; R4 cross-domain callback lifetime
=> tokens/epochs/DomainUnload; R5 game-control input leaks while typing => capture-owning fiber;
R6 memory 150-350 MB RSS => budgets/gates; R7 CEF collisions (WebUI etc.) => module scan + refusal;
R8 API resolution for LSPDFR authors => bundle-next-to-plugin + domain probe; R9 resolution/DPI/multi-
monitor; R10 game-update native drift => managed code only, codegen-centralized; R11 Enhanced/D3D12 OOS.

## 7. Critique log (2026-09-08)

External adversarial review (25 critiques). Accepted 25/25 -> changes: single load path; stacking
contract; CEF pin gate as P0; input/cursor/IME spec; event delivery contract + Pump; CefMessageRouter;
per-token queues; dirty rects; gamma shader; RSS/layer budgets; minidumps + own log (RPH log only for
lifecycle lines); version manifest/handshake; P1-in-game-gate first; SHVDN natives demoted to optional
spike. Full review notes in conversation archive; decisions above incorporate all fixes.

## 8. Logging policy

- Host + API + Bootstrap log to `SentinelUI\logs\sentinelui_*.log` (ring-buffered, severity levels,
  per-plugin tags, `sui log verbose` toggle). NEVER per-frame entries.
- RagePluginHook.log receives ONLY plugin load/unload lifecycle lines.
- Native crashes: own minidumps in the logs folder (RPH handler does not attribute ASI/native crashes).
