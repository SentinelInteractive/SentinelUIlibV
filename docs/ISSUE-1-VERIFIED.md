# Issue #1 — VERIFIED (in-game playtest, 2026-09-09)

Full loop confirmed in a live GTA V session (Legacy build 3889, RPH 1.131, LSPDFR 0.4.9).
KPI lines from `SentinelUI\logs\sentinelui_helper_20260909.log`:

```
cmd recv: first 'V' (10 bytes)        viewport command delivered
helper viewport -> 1914x1046          browser sized to the game window
cmd recv: first 'C' (2 bytes)         F10 capture reached the helper
cmd recv: first 'K' (11 bytes)        key events forwarded
cmd recv: first 'M' (9 bytes)         mouse motion forwarded (cursor moves)
cmd recv: first 'B' (12 bytes)        clicks forwarded
NUI_TEST:p1-button-clicked            page console message returned to the host
cmd recv: first 'U' (3 bytes)         typing forwarded (escape key)
```

Host side (`sentinelui_host_*.log`) mirrored the traffic:
```
cmd queued: first 'M' (10 bytes): M1287 869
cmd queued: first 'B' (13 bytes): B0 1 847 177
```

## KPIs
| Sphere | Status |
|---|---|
| Input capture (F10) freezes the game | ✅ |
| Software cursor tracks the mouse | ✅ |
| Page buttons receive clicks | ✅ |
| JS → host events (`NUI_...`) | ✅ (helper hop) |
| Viewport sync to game resolution | ✅ |
| Clean shutdown (hooks removed, helper stopped) | ✅ |

## Known follow-up
- The managed hop (Bootstrap mirrors `NUI_` events into RagePluginHook.log) is proven by the
  offline HostProbe round-trip; the in-game RPH mirror line was not observed this session — check
  under load or adjust the mirror verbosity during issue #3 work.
- Evidence excerpt above is verbatim from today's playtest logs.