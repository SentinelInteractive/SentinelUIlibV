# Plugin authoring quickstart

SentinelUI renders plain HTML/CSS/JS pages in-game (CEF, out-of-process) and gives your
plugin a small bridge to them. You bring the page; the runtime brings the pixels, input,
and the message plumbing.

## The whole API

| C# | JS (injected into every page as `window.sui`) |
|---|---|
| `Sui.CreateLayer(url)` / `CreateLayerFromHtml(html, baseUrl?)` | `sui.send(type, payload)` — fire-and-forget event to your plugin |
| `Sui.RegisterContentRoot(name, folder)` | `sui.on(type, handler)` — receive events (`'*'` for all) |
| `SuiLayer.ShowCaptured()` / `HideCaptured()` | `sui.request(type, payload, timeoutMs?)` — Promise, resolved by your plugin |
| `SuiLayer.Message` (events) / `SuiLayer.Request` (requests) | |
| `SuiLayer.Send(type?, payload)` | |
| `Sui.Pump()` | |

There is no component toolkit and no build step — plain web tech only.

## Rules (the four things that matter)

1. **Never call `Sui.Initialize`/`Sui.Shutdown` from a plugin.** The Bootstrap plugin owns
   the host lifecycle. Just use the API; the first call attaches your AppDomain as a client.
2. **Call `Sui.Pump()` once per game frame, on the game thread.** Events and requests are
   delivered there, so handlers may call game natives safely.
   - RPH plugin: start one `GameFiber` with a `while(true) { Sui.Pump(); GameFiber.Yield(); }` loop.
   - LSPDFR callout: `Sui.Pump();` in `Callout.Process()`.
3. **Show/hide through capture.** `layer.ShowCaptured()` shows the page and routes
   mouse/keyboard into it (the game freezes its controls; an `INPUT CAPTURE • F10 RELEASE`
   badge appears). `layer.HideCaptured()` releases. The player can also toggle with F10.
4. **Use the scheme for real files.** `Sui.RegisterContentRoot("myplugin", @"Plugins\MyPlugin\ui")`
   then `Sui.CreateLayer("sentinelui://myplugin/index.html")`. Files are re-read per request
   (`no-store`), so `layer.Reload()` is hot reload.

## A minimal plugin

```csharp
// RPH plugin Main() — do NOT call Sui.Initialize/Shutdown.
Sui.RegisterContentRoot("terminal", @"Plugins\MyTerminal\ui");
var layer = Sui.CreateLayer("sentinelui://terminal/index.html");
if (layer == null) { Game.LogTrivial("[MyTerminal] SentinelUI runtime not available"); return; }

layer.Request += req =>                       // runs from Sui.Pump() on the game thread
{
    if (req.Type == "ped.scanClosest")
    {
        var ped = World.GetClosestEntity(Game.LocalPlayer.Character.Position, 10f,
                                         GetEntitiesFlags.ConsiderHumanPeds) as Ped;
        req.Reply(new { name = ped?.Model.Name ?? "none", warrant = false });
    }
};

layer.Message += msg => Game.LogTrivial($"[MyTerminal] {msg.Type}: {msg.PayloadJson}");

GameFiber.StartNew(() =>
{
    layer.ShowCaptured();
    while (true)
    {
        Sui.Pump();
        GameFiber.Yield();
    }
});
```

Close from the page: `sui.send("ui.close")` → your `Message` handler calls
`layer.HideCaptured()` (and the page is free to re-show later).

## The page side

`window.sui` is injected before your scripts run; nothing to include in-game.

```html
<button id="scan">Scan closest</button>
<pre id="out"></pre>
<script>
  sui.on('ui.close', () => { /* ask the plugin to close; see below */ });

  document.getElementById('scan').onclick = async () => {
    try {
      const data = await sui.request('ped.scanClosest', { radius: 10 }, 4000);
      document.getElementById('out').textContent = JSON.stringify(data, null, 2);
    } catch (err) {
      document.getElementById('out').textContent = 'error: ' + err;
    }
  };

  sui.send('ui.ready', { version: 1 });   // plain event, no reply expected
</script>
```

Under the hood both sides exchange one JSON envelope — you never Touch it directly, but it
helps when debugging:

```json
{ "v": 1, "kind": "req|evt|res", "type": "ped.scanClosest", "id": 3, "ok": true, "payload": { } }
```

## Content and constraints

- `sentinelui://<name>/<path>` is local-only, traversal-safe, `no-store`, `script-src 'self'`
  (ship JS/CSS as files, no inline `<script>`).
- `CreateLayerFromHtml(html, baseUrl?)` is for small self-contained pages (segments travel
  through the pipe): a `data:` URL is used, and `baseUrl` is injected as `<base href>` so
  relative files can resolve against a registered root.
- Keep one interactive layer at a time: input capture is global. Click-through/HUD layers are
  fine alongside it (`SetClickThrough(true)`).
- Keyboard is ASCII + shortcuts (Ctrl+C/V work). IME/non-ASCII is not supported yet.
- Pages run in a shared CEF profile; don't put secrets in pages or send them to layers.

## Developing without the game

Load `samples/browser-dev/sui-bridge.js` before your page scripts: `sui.send/on/request` work
in a normal browser, and `window.suiDev` can fake request replies per type. The same page runs
unchanged in-game (the shim is only for the browser; in-game the real bridge is already there).

## Debugging

- Page errors/console: `console.log` forwards to your plugin as `Message`/events and to
  `<game>\SentinelUI\logs\sentinelui_helper_*.log`.
- Plugin side: `<game>\SentinelUI\logs\sentinelui_api_*.log` (API) and `sentinelui_host_*.log`.
- `layer.ExecuteJs("...")` is the escape hatch for one-off page probes.
- F9 hides the whole overlay; F11 toggles the demo HUD layer; F10 toggles capture.
