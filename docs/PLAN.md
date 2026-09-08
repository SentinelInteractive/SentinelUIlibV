# SentinelUI — Authoritative Plan

Status: **Phase 0 in progress**. Source of truth for architecture decisions, gates and acceptance.
Last updated: 2026-09-08 (after external design critique — all 25 critiques addressed; see §7).

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
MANAGED API     SentinelUI.API.dll (net472 x64, no SHVDN). Static facade, DllImport of host exports
                (or LoadLibrary by absolute path). Events delivered ONLY on the thread calling Pump()
                (game thread by contract => handlers may call natives). Per-domain registration tokens,
                AppDomain.DomainUnload auto-unregister, epoch/tombstone guards, strong delegate refs.
BOOTSTRAP       SentinelUI.Bootstrap.dll (RPH plugin, <game>\Plugins\). Sole host loader: GetModuleHandle
                guard, then LoadLibrary(<game>\SentinelUI\SentinelUI.asi). Per-frame pump fiber.
                Console commands (sui ...). Graceful shutdown on unload - NEVER in PROCESS_DETACH.
```

- **Single canonical load path**: Bootstrap only. Host file lives in `<game>\SentinelUI\` (a folder ASI
  loaders never scan) => no double-load, deterministic hook order.
- **Cross-domain**: no managed singletons across AppDomains (LSPDFR plugins vs Bootstrap). Only the native
  host state is shared (same pattern as RAGENativeUI's rnui_shared_state mmf).
- **Stacking guarantee** (tested): HTML above game HUD/pause menu; below RPH-drawn graphics content.
- **SHVDN custom natives**: optional 2-day spike, NOT in critical path.

## 3. Key decisions (post-critique)

1. Host: C++20, CEF pinned at Phase-0 gate result (LTS 144 first candidate), own thin D3D11 hook.
2. Load: RPH Bootstrap only; host in `<game>\SentinelUI\SentinelUI.asi`; GetModuleHandle guard.
3. Render: transparent OSR, dirty-rect partial uploads, per-layer FPS caps, redraw-on-invalidate,
   WasHidden + timer throttle for hidden layers. sRGB/gamma-correct quad shader.
4. Input: WndProc forwarding; capture-owning GameFiber (DISABLE_CONTROL_ACTION set) while a
   capture-focused layer is visible (managed side owns control blocking); virtual cursor + host cursor;
   focus-loss => synthesized key-ups + WasHidden; IME workstream (Phase 4 stretch).
5. Managed API: static facade; events queued per token; Pump() drains on game thread; auto-unregister on
   DomainUnload; plugin authors bundle API DLL next to their plugin.
6. Cross-domain: native-host-only shared state.
7. Content: sentinelui:// scheme, registered roots, local-only by default, CSP, hot reload.
8. Protocol: versioned JSON envelope; JS->host = CefMessageRouter; host->JS = postMessage;
   managed<->host = typed SPSC queues w/ caps + SuiPumpEvents.
9. Web toolkit: zero-framework custom elements (sui-panel/button/list/form/notify/...), CSS-variable
   theme, no build step. Auto-pump + no-host graceful degradation = mandatory DX requirements.
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
  NuGet (API DLL only).
- **P4 Web toolkit**: components, theme, browser-runnable gallery, hot reload, i18n-ready; optional
  JS->native request bridge using natives.json codegen.
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
