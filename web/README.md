# web: demo content for the `sentinelui://` scheme

Not a component toolkit. SentinelUI is a **generic renderer**: pages bring their own
HTML/CSS/JS, and plugin authors own their UI. This folder ships the runtime's own demo and
acceptance pages. `tools/pack.ps1` deploys them to `<game>\SentinelUI\content\`, where the
runtime serves them as `sentinelui://sui/<file>`:

| File | What it is |
|---|---|
| `p1_demo.html` / `p1_demo.js` | Boot layer (layer 1). The page paints nothing: it is the transparent base of the stack, and its script logs one `NUI_TEST:p1-boot bridge=...` line so the host log shows the base page loaded and got its bridge. |
| `demo_hud.html` / `demo_hud.js` | F11 acceptance strip. A click-through layer at z 10 that composites above the boot layer, shows the live link state and clock, and logs `NUI_HUD:hud-tick` once a minute. |
| `badge.html` / `badge.js` | Always-on-top badge (click-through at z 9000). Brand lockup, build status, live clock, and the game-process CPU read from the `sys.stats` request. |
| `selftest_page.html` | Helper self-test page, loaded from a filesystem path (`file://`) rather than the scheme. Box geometry matches the Phase-0 harness check, and the page logs `NUI_TEST:helper-alive`. Test surface only, not a design reference. |

## The protocol helper

The runtime injects `window.sui` into every page before its scripts run:

- `sui.send(type, payload)` fires an event at the host, no reply.
- `sui.on(type, handler)` subscribes (`'*'` receives every event) and returns an unsubscribe
  function. Handlers receive `(payload, envelope)`.
- `sui.request(type, payload, timeoutMs?)` returns a Promise the host resolves or rejects.

Envelopes are versioned JSON (`{v: 1, kind, type, id, payload}`). The host delivers them through
`window.sui._deliver`, which also reposts each one on `window.postMessage`. That is plumbing, not
a component library.

To build a page without launching the game, use `samples/browser-dev/`: `sui-bridge.js` stands
in for the injected bridge, and `demo.html` exercises every call.

## Constraints that bite

- The scheme CSP is `script-src 'self'`: ship JS as separate files, no inline `<script>`.
  Inline `<style>` is allowed.
- `fetch` and `XMLHttpRequest` do not work for `sentinelui://` URLs: Chromium does not support
  them for custom schemes. Load dynamic content with `<script src>`, an `<iframe>`, or a
  fragment bundled into a JS file.
- Responses are served `no-store`, so reloading a layer re-reads the files from disk.
- The runtime is offline and local: no webfonts, no CDNs, no external requests.

A `sui-*` component library (custom elements, theme, gallery) is **explicitly out of scope**: it
would make the renderer opinionated and duplicate plain web tech (issue #5, closed 2026-09-11).
Component libraries belong in userland.
