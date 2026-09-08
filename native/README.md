# native/ — CEF host (SentinelUI.asi) + worker subprocess

Not yet scaffolded (Phase 1+). Design constraints (see `docs/PLAN.md`):

- C++20, MSVC (VS 2026 toolset), CMake. CEF pinned to the Phase-0 gate result (LTS 144 first candidate).
- Host module ships as `<game>\SentinelUI\SentinelUI.asi`, loaded **only** by the RPH Bootstrap
  (`LoadLibrary` absolute path) — never self-loading via ASI loaders.
- Hooks: `IDXGISwapChain::Present/ResizeBuffers` vtable injection (installed after RPH's),
  `WndProc` chain. No `rage::scrThread::Run` hook.
- Rendering: transparent CEF OSR → staging buffer (own D3D11 context) → per-layer textures →
  gamma-correct quad composited in the Present hook. Dirty-rect partial uploads, per-layer FPS caps,
  `WasHidden` + timer throttle for hidden layers.
- Exports: flat C ABI `Sui*` (see `managed/SentinelUI.API/NativeHost.cs` for the v2 export table
  contract). Callbacks only enqueue into per-token SPSC queues; dispatch happens exclusively via
  `SuiPumpEvents()` from the game thread.
- Diagnostics: minidump handler + ring-buffered structured logs to `<game>\SentinelUI\logs\`.
- CEF collision pre-flight: scan loaded modules; refuse to init when another CEF host is present.
