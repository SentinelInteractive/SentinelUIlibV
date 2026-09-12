# web/ — demo content for the `sentinelui://` scheme

Not a component toolkit. SentinelUI is a **generic renderer**: pages bring their own
HTML/CSS/JS, and plugin authors own their UI. This folder only ships the in-game
demo/acceptance pages that `pack.ps1` deploys to `<game>\SentinelUI\content\`:

```
p1_demo.html / p1_demo.js      boot layer (P1/P2 acceptance: input, cursor, events, hot reload)
demo_hud.html / demo_hud.js    F11 overlay strip (layer stacking + click-through)
selftest_page.html             helper self-test page (file:// baseline, tests only)
```

A `sui-*` component library (custom elements, theme, gallery) is **explicitly out of
scope** — it would make the renderer opinionated and duplicate plain web tech
(issue #5, closed 2026-09-11). Component libraries belong in userland.

The runtime *does* inject `window.sui` into every page — a ~40-line protocol helper
(`send` / `on` / `request`) that carries the versioned envelope between the page and the
plugin. That is plumbing, not a component library; see `docs/PLUGIN-AUTHORING.md`.
