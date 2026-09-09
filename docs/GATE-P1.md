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
| Alt-tab / WM_KILLFOCUS | ⏳ | Observed benign log entries; no stuck input seen |
| Borderless ⇄ fullscreen / resolution change | ⏳ | ResizeBuffers path implemented; untested in-game |
| Steam overlay / OBS on/off | ⏳ | Untested |
| RAGENativeUI menu (F11) coexistence | ⏳ | Untested |
| LSPDFR on-duty ⇄ off-duty with layer open | ⏳ | Untested |
| 20× consecutive restarts (exit/leak) | ⏳ | Untested |
| DXGI device-lost recovery | ⏳ | Untested |
| BSOD-free at shutdown | ✅* | One 0xD1 (driver) occurred mid-work; provenance under review, GPU churn reduced |

Legend: ✅ verified in-game · ⏳ pending matrix pass.