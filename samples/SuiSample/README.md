# SuiSample — quick LSPDFR callout for SentinelUI

A minimal LSPDFR callout that opens a SentinelUI panel built from **inline HTML** and
receives its button events — testing the cross-AppDomain client path (the callout runs
in LSPDFR's AppDomain, not Bootstrap's).

## Build

```powershell
dotnet build            # uses the GameDir default in SuiSample.csproj
# or: dotnet build -p:GameDir="X:\path\to\Grand Theft Auto V"
```

## Deploy

```powershell
pwsh -File deploy.ps1            # refuses while the game runs
# or: pwsh -File deploy.ps1 -GameDir "X:\path\to\Grand Theft Auto V"
```

It copies `SuiSample.dll` + `SentinelUI.API.dll` into `<game>\Plugins\LSPDFR\`.

## Try it in-game

1. Launch GTA V via `RagePluginHook.exe`; LSPDFR loads the pack from `Plugins\LSPDFR\`
   (`[SuiSample] pack loading...` in `RagePluginHook.log`).
2. Go **on duty** (LSPDFR console F4 → `ForceDuty`, or the LSPDFR menu) — the pack then
   registers the callout (`[SuiSample] registered callout with LSPDFR.`).
3. Open the console (F4) and type:
   ```
   ForceCallout SuiSample
   ```
   (probability is High, so it can also spawn on its own.)
4. The callout starts, the panel appears on the left with an Acknowledge button.
5. Press **F10** (input capture), click **Acknowledge** — the callout logs the event and
   shows a game notification. Click **Close callout** or press **End** to finish.

## What to look for

- `RagePluginHook.log`:
  - `[SuiSample] pack loading...` and `[SuiSample] registered callout with LSPDFR.`
  - `[SuiSample] callout accepted; attaching SentinelUI client...`
  - `[SuiSample] layer created (native id ...)`
  - `[SuiSample] page event: NUI_SAMPLE:ack` — the button event crossed domains.
- `<game>\SentinelUI\logs\sentinelui_api_*.log`: `Event client registered (token ...)`.
- The Bootstrap overlay keeps working while the callout panel is open (two layers,
  two domains, independent event queues).

## How it differs from a bare callout DLL

LSPDFR does **not** auto-discover `[CalloutInfo]` types in `Plugins\LSPDFR\`. The assembly
must expose an RPH `Plugin` class (`Main.cs`) which calls
`Functions.RegisterCallout(typeof(SuiSampleCallout))` — that is the pattern your
`SentinelCalloutsV` pack uses.

## Uninstall

Delete `SuiSample.dll` + `SentinelUI.API.dll` from `<game>\Plugins\LSPDFR\`.
