# browser-dev: design pages without the game

SentinelUI pages are plain HTML/CSS/JS, and in-game the runtime injects `window.sui` into every
page before its own scripts run. This folder is the browser stand-in for that bridge: build the
page here, click it through here, then ship the same file.

| File | What it is |
|---|---|
| `sui-bridge.js` | The shim. Implements `window.sui` (`send`, `on`, `request`, `_deliver`) the way the runtime does. Load it before your page scripts. |
| `demo.html` / `demo.js` | A demo page that walks every path: request/reply, fire-and-forget, a fake answer per type, a missing handler, a timeout, and a push arriving from the host. |

Open `demo.html` in any Chromium-based browser straight from disk (double-click works; nothing
needs a server). Each control reports what came back in the panel below it.

## Use it in your own page

```html
<script src="sui-bridge.js"></script>
<script src="app.js"></script>
```

Requests that need an answer get one from `window.suiDev`, keyed by request type:

```js
window.suiDev = {
  'ped.scanClosest': function (payload) { return { name: 'Dev Ped', warrant: false }; },
  'vehicle.lookup': async function (payload) { return { plate: payload.plate, stolen: false }; }
};
```

A handler may return a value, return a promise, or throw. A type with no entry rejects, which is
the state a page should show when no plugin is behind it.

## The API in one screen

| JS, available in every page | What it does |
|---|---|
| `sui.send(type, payload)` | fires an event at the host; no reply |
| `sui.on(type, handler)` | subscribes; `'*'` receives every event; returns an unsubscribe function |
| `sui.request(type, payload, timeoutMs?)` | Promise the host resolves or rejects; rejects with `sui request timeout: <type>` once `timeoutMs` passes |
| `sui._deliver(envelope)` | the runtime's host-to-page entry point; a page can call it to simulate a push |

Every call is one versioned envelope:

```
page -> host   { v: 1, kind: 'evt' | 'req', type, id, payload }
host -> page   { v: 1, kind: 'res', type, id, ok, payload }   or   { v: 1, kind: 'evt', type, payload }
```

Handlers receive `(payload, envelope)`, and the envelope is also reposted on `window.postMessage`
for pages that prefer raw listeners.

## What the shim cannot do

- No game or plugin data. `sys.stats` (the game-process CPU the shipped badge polls) is answered
  by the runtime's Bootstrap in-game, so in a browser it rejects like any other unmapped type.
- No input capture, no F10, no layer stacking, no click-through. Those are runtime behaviours,
  not page behaviours, so the browser proves the page's own logic and the runtime proves the rest.
- Rendering goes through CEF off-screen rendering in-game. Check the look there before shipping;
  the browser is for structure, states and copy.

## Constraints that apply to the shipped page too

- The scheme CSP is `script-src 'self'`: ship JS as separate files, no inline `<script>`.
  Inline `<style>` is allowed.
- `fetch` and `XMLHttpRequest` do not work for `sentinelui://` URLs. Load dynamic content with
  `<script src>`, an `<iframe>`, or a fragment bundled into a JS file.
- Content is served with `no-store`, so reloading a layer re-reads it from disk.
- The runtime is offline and local: no webfonts, no CDNs, no analytics.
