# samples: working examples

Two examples that build against the runtime in this repository. They are development
material, not distribution artifacts; build and deploy notes live in each sample's README.

| Sample | What it shows |
|---|---|
| [`SuiSample/`](SuiSample/README.md) | LSPDFR callout pack that opens an inline-HTML SentinelUI panel from LSPDFR's own AppDomain: attach as a client, pump that domain's events, never call `Initialize`/`Shutdown`. |
| [`browser-dev/`](browser-dev/) | `window.sui` shim plus a demo page, so a panel can be written and clicked through in a normal browser before it runs in-game. |
