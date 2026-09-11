# tools/ — build, packaging, stress testing and codegen

| Script | Purpose |
|---|---|
| `init-cef.ps1` | Download a pinned CEF distribution (cef-builds.spotifycdn.com), extract to `native/third_party/cef`, copy wrapper sources; records version in `version.json` |
| `build-host.ps1` | Build `native\host` → `SentinelUI.asi` (CMake + Ninja + MSVC) |
| `build-helper.ps1` | Build `native\helper` → `SentinelUI_Host.exe` + CEF runtime |
| `dotnet build managed\SentinelUI.sln` | Build API + Bootstrap + HostProbe (net472 x64, no build script needed) |
| `pack.ps1` | Deploy the runtime into a GTA V install (host, CEF runtime, content, managed DLLs); refuses while GTA5 runs |
| `stress.ps1` | Unattended N-session acceptance loop (issue #4): launch via RPH, scenario, RSS/orphan/dump checks, CSV+JSON results. See `docs/STRESS-TEST.md` |
| `scenario.ps1` / `scenario-lib.ps1` | In-session scenarios (LayerChurn, CaptureAltTab, Resize, DeviceLost, GameBar, HudTicks, QuitGame); `scenario.js`-shared with the stress loop |
| `codegen` (P4) | Generate typed native wrappers / JS signature metadata from `docs/data/natives.json` |
