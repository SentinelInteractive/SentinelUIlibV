# SentinelUI — Phase 0 result (2026-09-08)

## CEF version pin — DECIDED: CEF 144 LTS (Chromium 144.0.7559.261)
Distribution: `144.0.34+g8fc21c8+chromium-144.0.7559.261_windows64` (standard)
- **sha1** `81e96c3efe8ea51de5ff8aba124eac5b18aabea5`, extracted under `native/third_party/cef/`
- Pin state: `native/third_party/cef-current.json` (tracked)

### Why 144 (LTS branch 7559) over 151/152
- The research review flagged OSR/Alloy churn in CEF 126+ (Alloy bootstrap deleted at M128, main-thread/UI-thread split at M130+, issue #3293 tracking OSR). **144 is a long-lived branch**; 151/152 are newer but the OSR path needs the same empirical proof before any upgrade.
- **Upgrade policy**: every CEF branch bump re-runs this harness (plus the in-game P1 gate) — a controlled release event, never a dependency bump.

## Gate harness evidence
`native/harness/` (`osr_harness.cpp`, builds via `tools/build-harness.ps1`)

Experiment (windowless OSR, `--disable-gpu`, software compositing, dedicated CEF thread,
`CefDoMessageLoopWork` pump @ 8 ms, 1280×720 transparent page, D3D11 upload/readback):

| Q | Result |
|---|---|
| Q1 dedicated-thread `CefInitialize` | PASS (init 156–231 ms) |
| Q2 transparent OSR `OnPaint` | PASS (frame alpha verified: bg=0, tile≈166) |
| Q3 `CefDoMessageLoopWork` pump | PASS (entire run used the pump, no message loop) |
| Q4 JS→host console channel | PASS (`NUI_TEST:hello-from-page` observed) |
| Q5 D3D11 texture roundtrip | PASS (700 px compared, 0 mismatches) |

Perf observations (static page):
- first paint 137–1049 ms after init (system-load dependent; cold cache slower)
- after load a static page **paints then goes idle** — zero frame traffic with no changes
  (critical for hidden/background layers)

Artifacts (local, gitignored): `native/harness/build/Release/osr_harness.exe`,
`harness_frame.ppm` (visual check), `harness_trace.txt`, `debug.log`, `harness_cef_cache/`.

## Decisions recorded
- D1 confirmed: host = C++20 + CEF (pinned per above).
- D2 confirmed: single load path via RPH Bootstrap; dedicated CEF thread + pump (Q1/Q3 proof).
- D3 confirmed: software OSR + dirty-rect pipeline; static-page idle behavior proven.
- D7 confirmed: file→custom-scheme content delivery modeled on the harness `file://` load.

## Next
- P1 in-game host gate: swapchain/WndProc hooks, one layer, toggle key — needs the game install
  (RPH 1.131 + LSPDFR 0.4.9) at D:\SteamLibrary\...
- Optional: re-run harness on 151.3.24 to document upgrade delta (not required for the pin).