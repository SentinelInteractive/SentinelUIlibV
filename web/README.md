# web/ — SentinelUI web component toolkit

Not yet scaffolded (Phase 4). Planned contents — zero-framework, zero-build custom elements:

```
components/
  sui-panel       sui-button      sui-list       sui-form
  sui-input       sui-notify      sui-badge      sui-progress    sui-keybind
sui-theme.css     (CSS-variable theme, GTA/LSPDFR-styled presets)
sui.js            (message-envelope runtime, focus/blur visibility, hot reload)
gallery/          (component gallery — runs in a normal browser too)
```

Design rules:
- Web Components standard (custom elements + shadow DOM), vanilla JS, no framework, no build step.
- Runtime communicates through the versioned envelope `{v,id,layer,type,payload}`:
  JS→host via CefMessageRouter queries; host→JS via `window.postMessage`.
- Content is served from `sentinelui://<plugin>/<path>` — local-only by default, CSP-tight.
- DX requirement: consumers must never care about the pump; pages are plain web pages.
