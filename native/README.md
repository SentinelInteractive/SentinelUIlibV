# native/: the in-game compositor and the CEF host

Two binaries plus a test stub:

- `host/`: **SentinelUI.asi**, the thin compositor that runs inside the game. Hooks
  `IDXGISwapChain::Present`/`ResizeBuffers` (stacked on top of RPH's own Present hook) and the
  game window's `WndProc`, consumes shared-memory frames and composites them as gamma-correct
  quads in layer order.
- `helper/`: **SentinelUI_Host.exe**, the dedicated CEF process. Transparent OSR rendering,
  frame publishing over named shared memory, the `sentinelui://` scheme, and the named-pipe
  command/event channel with the host.
- `hoststub/`: a fake host used by the managed load-path probe (`HostProbe`); not shipped.

Constraints that shape all of it:

- C++20, MSVC + CMake, CEF pinned to the LTS 144 build recorded in `version.json`.
- `SentinelUI.asi` must be loaded from the game root by an ASI loader (ScriptHookV) at process
  start; the RPH Bootstrap only attaches to it. RPH's hook guard reverts foreign Present hooks
  installed after its own.
- CEF runs in its own EXE process by design: embedding it in a DLL inside the game process
  failfasts on the first libcef call, and the split also means a page or browser crash can
  never take the game down.
- Exports are a flat C ABI `Sui*` table (versioned via `SuiGetApiVersion`); callbacks enqueue
  into per-token SPSC queues and are dispatched only through `SuiPumpEvents()` on the game
  thread.
- Diagnostics: minidump capture plus structured logs under `<game>\SentinelUI\logs\`.

Build with `tools\build-host.ps1` and `tools\build-helper.ps1` (the CEF runtime is acquired by
`tools\init-cef.ps1`).
