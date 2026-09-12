# browser-dev — design pages without the game

Open `demo.html` in any browser: `window.sui.send/on/request` behave like the in-game
bridge, so the page you build here is the page you ship (only `sui-bridge.js` is
browser-only — in-game the runtime injects the real bridge).

- `sui-bridge.js` — drop-in shim; load it before your page scripts.
- `demo.html` — minimal example: request/reply, fire-and-forget send, host-push handling.

Fake data for requests: set `window.suiDev = { 'ped.scanClosest': payload => ({...}) }`
before the first request; unmapped types echo back after 150 ms.

See `docs/PLUGIN-AUTHORING.md` for the full API and constraints.
