# SuiSample: LSPDFR callout with an inline SentinelUI panel

A minimal LSPDFR callout that opens a SentinelUI panel and exchanges messages with it. The
panel HTML lives in `SuiSampleCallout.cs`, so the sample is one assembly and one deploy step.

It exercises the **cross-domain client path**: the callout runs in LSPDFR's AppDomain, so it
attaches to the already-loaded SentinelUI host as a client (never `Initialize`/`Shutdown`),
creates an inline-HTML layer, calls `Sui.Pump()` in `Callout.Process()` to receive that
domain's events and requests, and closes the layer in `End()`.

| File | What it holds |
|---|---|
| `Main.cs` | RPH `Plugin` entry point: registers the callout when the player goes on duty, plus an `AssemblyResolve` hook that loads `SentinelUI.API.dll` from the deploy locations (game root, `Plugins\`, `SentinelUI\`). |
| `SuiSampleCallout.cs` | The callout and the panel HTML it creates. |
| `deploy.ps1` | Builds the project and copies `SuiSample.dll` into `<game>\Plugins\LSPDFR\`. |

## What the panel does

- On open it asks the plugin for scene data (`sui.request('sample.scene')`) and shows the
  street name and the player position, with loading, empty (no street name at that spot) and
  error states.
- **Acknowledge** sends `sui.request('sample.ack')`; the callout answers with the local time
  it handled the request, the panel shows that time, and a game notification appears.
- **Close callout** sends `sui.send('sample.close')`, which ends the callout. The End key
  does the same.
- The status strip mirrors the `window.sui` link state (LIVE / OFFLINE) and shows the clock.

The page uses `window.sui` only, so the same HTML also runs in a browser with
`samples/browser-dev/sui-bridge.js` loaded: define `window.suiDev` replies for `sample.scene`
and `sample.ack` and the panel behaves as it does in-game. The panel follows the repository's
design direction (dark dispatch panel, matte surfaces, one amber accent).

## Build

```powershell
dotnet build
# or, when the game is installed elsewhere:
dotnet build -p:GameDir="X:\path\to\Grand Theft Auto V"
```

`GameDir` defaults to `D:\SteamLibrary\steamapps\common\Grand Theft Auto V` (see
`SuiSample.csproj`). The build reads two assemblies from that folder:

- `<game>\Plugins\LSPD First Response.dll` (LSPDFR)
- `<game>\SentinelUI.API.dll` (SentinelUI runtime, already deployed)

## Deploy

```powershell
pwsh -File deploy.ps1
# or: pwsh -File deploy.ps1 -GameDir "X:\path\to\Grand Theft Auto V"
```

The script stops if GTA5 is running, builds unless you pass `-SkipBuild`, and copies
`SuiSample.dll` into `<game>\Plugins\LSPDFR\`. It also removes a stale `SentinelUI.API.dll`
from that folder: LSPDFR scans it for callouts, while the API is loaded from the game root by
the resolver in `Main.cs`.

## Try it in-game

1. Launch GTA V with `RagePluginHook.exe`. LSPDFR loads packs from `<game>\Plugins\LSPDFR\`,
   and `RagePluginHook.log` shows `[SuiSample] pack loading...`.
2. Go on duty (F4 console: `ForceDuty`, or the LSPDFR menu). The pack then logs
   `[SuiSample] registered callout with LSPDFR.` and shows a notification.
3. F4 console: `ForceCallout SuiSample`. The callout probability is Low, so it can also
   start on its own.
4. The panel opens near the top left. Press **F10** for input capture, then click a button.
   Acknowledge logs a request that the callout answers; Close callout or the **End** key
   closes the panel.

## What to look for in the logs

`RagePluginHook.log`:

- `[SuiSample] pack loading...`
- `[SuiSample] registered callout with LSPDFR.`
- `[SuiSample] callout accepted; attaching SentinelUI client...`
- `[SuiSample] layer created (native id ...).`
- `[SuiSample] page request: sample.scene` and `[SuiSample] page request: sample.ack`
- `[SuiSample] page event: sample.close null`
- `[SuiSample] layer closed.`

`<game>\SentinelUI\logs\sentinelui_api_*.log`: `Event client registered (token ...)`.

The Bootstrap overlay keeps working while the callout panel is open: two layers, two
AppDomains, one event queue each.

## Why the pack needs a Plugin class

LSPDFR does not discover `[CalloutInfo]` types on its own. The assembly must expose an RPH
`Plugin` class (`Main.cs`) that calls `Functions.RegisterCallout(typeof(SuiSampleCallout))`,
which this sample does when the player goes on duty.

## Uninstall

Delete `SuiSample.dll` from `<game>\Plugins\LSPDFR\`.
