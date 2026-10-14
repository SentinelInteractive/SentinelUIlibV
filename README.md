<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="assets/sentinel-interactive-dark.svg">
    <img src="assets/sentinel-interactive.svg" alt="Sentinel Interactive" width="360">
  </picture>
</p>

# SentinelUI

SentinelUI renders ordinary web pages on top of GTA V. A plugin authors its interface in HTML, CSS and JavaScript, and the runtime draws that page over the game with Chromium. Plugins do not need ScriptHookVDotNet, scaleforms or a UI component toolkit: a plugin brings a web page, and SentinelUI brings the rendering, input capture and the message channel between the page and the plugin code.

SentinelUI targets RagePluginHook and LSPDFR plugins. It is part of Sentinel Interactive (project `0x52504637`).

## Status

SentinelUI is pre-alpha. The following work in-game on GTA V Legacy:

- The overlay renders: a CEF page reaches the screen as a D3D11 quad composited from the `Present` hook. F9 shows and hides it.
- Input capture works with a software cursor (F10), click-through layers, z-order and per-layer focus.
- Pages and plugin code exchange messages through `send`, `on` and `request` over a versioned JSON envelope.
- The `sentinelui://` scheme serves plugin pages from disk with hot reload, and layers can also be created from inline HTML.
- Every plugin AppDomain attaches as a client while one Bootstrap instance owns the host.
- Diagnostics are available through `Sui.Status`, `Sui.Diagnose()` and the `sui status` / `sui diag` console commands.

The stability matrix (borderless and fullscreen, Steam overlay, RAGENativeUI coexistence, LSPDFR duty cycles, long restart runs) and release packaging are still open.

## How it works

| Piece | Role |
| --- | --- |
| Web layer | The plugin's HTML/CSS/JS, served from `sentinelui://<plugin>/`. |
| Render process | `SentinelUI_Host.exe`, a dedicated CEF process that renders pages offscreen and publishes frames through shared memory. |
| Compositor | `SentinelUI.asi`, which hooks the game's `Present` and draws the visible layer stack. |
| Managed API | `SentinelUI.API.dll` (net48), the facade plugins program against. |
| Bootstrap | `SentinelUI.Bootstrap.dll`, the RagePluginHook plugin that owns the runtime lifecycle. |

CEF runs in its own process on purpose. Embedding it in a DLL inside the game process crashes with a failfast on the first libcef call (`0xC0000409`), and the process split also means a page or browser crash cannot take the game down.

## Requirements

- GTA V Legacy (D3D11), Steam build 1.0.3889.x
- RagePluginHook (RPH) 1.131.x with LSPD First Response 0.4.9; the minimum SDK versions are recorded in `version.json`
- ScriptHookV as the ASI loader
- Windows 10 or 11, x64, with .NET Framework 4.8

`SentinelUI.asi` must stay in the game root. ScriptHookV loads it at process start so it can hook the swapchain before RPH arms its Present-hook guard, and Bootstrap only attaches to the loaded module.

## Build

```
dotnet build managed\SentinelUI.sln

tools\init-cef.ps1        # one-time: downloads the pinned CEF build (see version.json)
tools\build-host.ps1      # builds native\host   -> SentinelUI.asi
tools\build-helper.ps1    # builds native\helper -> SentinelUI_Host.exe

tools\pack.ps1            # deploys into your GTA V folder; refuses while the game is running
```

`pack.ps1` installs the runtime like this:

```
<game>\SentinelUI.asi          the ASI compositor, loaded by ScriptHookV at process start
<game>\SentinelUI_Host.exe     the CEF process
<game>\libcef.dll, *.pak ...   the CEF runtime, flat next to the ASI
<game>\SentinelUI\             content, logs and cache
<game>\Plugins\                SentinelUI.Bootstrap.dll and SentinelUI.API.dll
```

## Quick start

A plugin registers its content, creates a layer, answers page requests and pumps events once per frame:

```csharp
// RPH plugin. Bootstrap owns the host: never call Sui.Initialize or Sui.Shutdown.
Sui.RegisterContentRoot("terminal", @"Plugins\MyTerminal\ui");
var layer = Sui.CreateLayer("sentinelui://terminal/index.html");
if (layer == null) return;   // runtime not installed; keep the plugin running

layer.Request += req =>
{
    if (req.Type == "ped.scan") req.Reply(new { name = "..." });
};

GameFiber.StartNew(() =>
{
    layer.ShowCaptured();
    while (true) { Sui.Pump(); GameFiber.Yield(); }
});
```

On the page side, the runtime injects `sui.send(type, payload)`, `sui.on(type, handler)` and `sui.request(type, payload, timeoutMs)` before the first script runs. To build a page in a normal browser first, use the shim in `samples/browser-dev/`.

Game keys: F9 hides and shows the overlay, F10 toggles input capture, F11 spawns the demo HUD layer.

## Repository layout

| Path | Contents |
| --- | --- |
| `native/host/` | `SentinelUI.asi`: the in-game compositor that hooks `Present` and the window procedure and draws the visible layers. |
| `native/helper/` | `SentinelUI_Host.exe`: the CEF process that renders pages offscreen, serves `sentinelui://` and talks to the host over a named pipe. |
| `native/hoststub/` | A fake host used by the managed load-path probe; not shipped. |
| `native/third_party/` | The CEF SDK, fetched by `tools\init-cef.ps1` (not checked in). |
| `managed/SentinelUI.API/` | The plugin-facing API (net48). This is the assembly plugins reference. |
| `managed/SentinelUI.Bootstrap/` | The RPH plugin that owns the runtime lifecycle and the `sui` console commands. |
| `managed/tests/HostProbe/` | A console probe for the load path and interop marshalling; runs without the game. |
| `web/content/` | The demo pages shipped with the runtime: boot layer, HUD strip, badge and self-test page. |
| `samples/SuiSample/` | A working LSPDFR callout pack that opens a SentinelUI panel. |
| `samples/browser-dev/` | A `window.sui` shim and demo page for developing panels in a normal browser. |
| `tools/` | Build, deploy and stress scripts. |
| `assets/` | Logo artwork used by this README. |

`native/`, `web/`, `samples/` and `tools/` each have a README with the details.

## Contributing

The code style lives in `CODE-STYLE.md`: formatting is wired into the config files named there, and the page covers the conventions tools cannot check. Changes to the runtime should come with evidence (logs, repro steps or a gate note), not just a diff.

## License

SentinelUI is source-available under the PolyForm Strict License 1.0.0 (see `LICENSE`). You can read the source and learn from it, and use it for personal, noncommercial purposes such as hobby use, study and experiments. The license does not allow distributing it, modifying it, or publishing new works based on it. For anything beyond those terms, contact the maintainer first.
