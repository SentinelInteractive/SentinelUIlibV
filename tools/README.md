# tools/ — build, packaging and codegen

Planned scripts (Phase 0 starts `init-cef`):

| Script | Purpose |
|---|---|
| `init-cef.ps1` | Download a pinned CEF distribution (cef-builds.spotifycdn.com), extract to `native/third_party/cef`, copy wrapper sources; records version in `version.json` |
| `build-managed.ps1` | `dotnet build` API + Bootstrap (net472 x64), asserts output layout |
| `pack.ps1` | Assemble `<game>\SentinelUI\` runtime layout (host, cef\ runtime, worker, content, logs) |
| `codegen` (P4) | Generate typed native wrappers / JS signature metadata from `docs/data/natives.json` |
