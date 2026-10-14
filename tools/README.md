# tools: build, packaging, stress testing and codegen

| Script | Purpose |
|---|---|
| `init-cef.ps1` | Download a pinned CEF distribution (cef-builds.spotifycdn.com), extract it to `native/third_party/cef/<version>`, and write `native/third_party/cef-current.json` for the build scripts |
| `build-host.ps1` | Build `native\host` → `SentinelUI.asi` (CMake + Ninja + MSVC) |
| `build-helper.ps1` | Build `native\helper` → `SentinelUI_Host.exe` + CEF runtime |
| `build-hoststub.ps1` | Build the fake host used by the managed load-path probe |
| `build-harness.ps1` | Internal-only: builds the CEF OSR validation harness, whose sources are not part of this repo |
| `dotnet build managed\SentinelUI.sln` | Build API + Bootstrap + HostProbe (net48 x64, no build script needed) |
| `pack.ps1` | Deploy the runtime into a GTA V install (host, CEF runtime, content, managed DLLs); refuses while GTA5 runs |
| `stress.ps1` | Unattended N-session acceptance loop: launch via RPH, scenario, RSS/orphan/dump checks, CSV+JSON results in `tools\stress-results\` |
| `scenario.ps1` / `scenario-lib.ps1` | In-session scenarios (LayerChurn, CaptureAltTab, Resize, DeviceLost, GameBar, HudTicks, QuitGame); `scenario-lib.ps1` also backs the stress loop |
| `codegen` (planned) | Generate typed native wrappers and JS signature metadata from the natives catalog; not implemented yet |
