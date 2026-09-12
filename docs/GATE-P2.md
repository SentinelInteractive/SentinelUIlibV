# P2 acceptance gate — issue #2 (layer stack + sentinelui:// scheme)

Formal acceptance from issue #2. Status 2026-09-10 (after in-game playtests).

| Check | Status | Notes |
|---|---|---|
| Layer model exports (create/close/show/hide/z/click-through/focus) | ✅ | Export table v3; HostProbe round-trips z + click-through + visibility |
| Plugin registers a content folder, loads HTML/JS/CSS via the scheme | ✅ | HostProbe: temp root + `sentinelui://probe/issue2.html` → doc, CSS and JS all served |
| CSP-tight content | ✅ | `script-src 'self'`; pages ship external JS (p1_demo.js, demo_hud.js) |
| Directory traversal blocked | ✅ | `..` segments refused handler-side; URL normalization keeps requests in-root; containment re-checked on the resolved path |
| Hot reload | ✅ | Files read per request + `Cache-Control: no-store`; `SuiLayer.Reload()` / `L<id>` command (explicit in-game edit not separately recorded) |
| No `file://` in the content path | ✅ | Boot layer = `sentinelui://sui/p1_demo.html`; harness selftest keeps file:// (test-only) |
| Per-layer event routing (`<id>\|kind:body`) | ✅ | `SuiLayer.Message` fires only for the owning layer |
| J-during-load robustness | ✅ | Queued page JS runs after load commit |
| In-game: scheme-served demo renders | ✅ | 2026-09-10 session: helper log `layer 1 browser created: sentinelui://sui/p1_demo.html`, `p1_demo.js` served, no file:// |
| In-game: two layers, correct z-order | ✅ | Boot layer (z 0) + F11 HUD (z 10), both textures 1914×1046, HUD composited last |
| In-game: click-through routing | ✅ | With the click-through HUD open, F10 clicks reached the boot layer's Ping button (`NUI_TEST:p1-button-clicked` ×3) — `MouseTarget()` skipped the click-through layer |
| In-game: input capture + JS bridge | ✅ | K/M/B/U commands + console events in the helper log; game controls frozen while captured |
| In-game: per-layer viewport quality | ✅ | New layers adopt the game viewport on creation (`layer N texture 1914x1046`) |
| In-game: F9/F11 toggles | ✅ | F9 deterministic (both layers, one Present, hidden pages keep their last good frame); F11 instant show/hide |
| Layer budget: hidden layer ≈ zero cost / ≤3 visible | ⏳ | P5 gate (pages are `WasHidden` while the overlay is hidden) |

Legend: ✅ verified (offline HostProbe and/or the 2026-09-10 in-game sessions) · ⏳ later phase.

## Field findings from the in-game pass (all fixed)

**2026-09-12 (operator-verified in-game):** click dispatch fixed (`SendMouseClickEvent`'s
`mouseUp` param was inverted — buttons fired every other click), capture badge + rebuilt
cursor verified, F9/F10/F11 no longer forwarded to pages, and the swapchain-recreation blink
fixed (resources re-init now force-re-uploads every layer's resident frame in the same
Present). LSPDFR sample callout: 5/5 button events delivered across domains.1. **`ERR_UNKNOWN_URL_SCHEME` on first in-game navigation** — CEF child-process scheme
   registration raced the boot navigation under game-boot load. Fixed: register right
   after `CefInitialize`, settle window, bounded retry, per-process
   `scheme_registration.log`.
2. **Event line corruption under load** — overlapped reads were re-issued while pending and
   waiter events were manual-reset. Fixed in both pipe loops.
3. **F11 layer pixelated** — layers created after the viewport sync kept the 1280×720
   default and were upscaled. Fixed: `LayerManager` remembers the last viewport.
4. **F9 multi-layer stagger / ~1 s blank on spam** — `WasHidden` makes CEF emit an empty
   frame; publishing it blanked the layer until the next repaint. Fixed: hidden layers
   keep their last good frame; the host force-uploads it instantly on show.
5. **Orphan helper processes after an unclean game exit** (blocked a redeploy) — helper now
   exits 30 s after the pipe drops; `pack.ps1` refuses to deploy while GTA5 runs and
   preserves logs/cache.

## How to verify in-game (operator notes)

1. `tools\pack.ps1` → launch GTA V (Legacy, RPH).
2. Boot layer (`sentinelui://sui/p1_demo.html`) shows the P2 demo panel.
3. F10 → capture: buttons clickable, `NUI_TEST:p1-button-clicked` reaches
   `Sui.MessageReceived` (Bootstrap mirrors `NUI_*` lines into RagePluginHook.log).
4. F11 → demo HUD strip appears ABOVE the panel (z 10) and is click-through:
   clicks fall through to the panel buttons beneath.
5. Edit `SentinelUI\content\p1_demo.js`, press F9 twice (hide/show) — the change
   is live without restarting the game (hot reload, no-store).
