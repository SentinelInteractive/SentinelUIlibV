# GTA V / RPH / CEF — Lessons Learned (field log)

Hard-won knowledge from building SentinelUI (2026-09-08). Every item below was
**verified empirically** (mostly on the user's install: GTA V Legacy, Steam build
1.0.3889, RagePluginHook 1.131.1424.17745, LSPD First Response 0.4.9).

---

## 1. GTA V specifics

- **Legacy vs Enhanced**: the single-player modding scene for the classic game is the
  **Legacy build** (D3D11). Enhanced is D3D12 and out of scope for this kind of overlay.
- **Window class is `grcWindow`** — that's how you find the game's top-level window
  (EnumWindows + class name) for WndProc subclassing. It matched first try in-game.
- **`dinput8.dll` in the game root = the ASI loader hook** (OpenIV-style). ASIs are auto-loaded
  **from the main game directory only** (and `scripts\` for some loaders). A `.asi` in a
  subfolder will NOT auto-load — if you need ASI behavior, it goes in the root.
- **Two game copies possible**: Steam + Epic (e.g. `F:\epic games\GTAV`) — the Epic path also
  contains a **space**, which already bit our CLI arg parsing.
- **GTA V's folder name contains spaces** (`…Grand Theft Auto V`) — treat every path as
  space-bearing. Quote everything, parse quotes everywhere.
- Backbuffer can be **non-standard** (observed 1914×1046) — never assume 1080p.
- The game's own HUD/pause content lands in the backbuffer before Present → an overlay drawn
  in a Present hook sits **above the game output** (below RPH-drawn graphics content if RPH
  hooked the same Present earlier/later — verify platform specifics yourself).

## 2. RagePluginHook / LSPDFR ecosystem

- **Plugin isolation**: RPH loads each plugin into its **own AppDomain whose probing path is the
  GAME ROOT, not the plugin folder**. A dependency DLL sitting next to the plugin is invisible —
  resolve it explicitly (`AppDomain.AssemblyResolve`) or place it at the game root
  (the RAGENativeUI convention).
- **RPH hooks the game's swapchain itself** (its launcher log lists `D3D11Present:`,
  `D3D11ResizeBuffers:` addresses). Third-party overlays must therefore **stack** their Present
  hook with RPH's — function-level detours (MinHook on the resolved vtable addresses) coexist
  cleanly; order determines stacking.
- **Plugins live in `<game>\Plugins\`**; LSPDFR itself is such a plugin; LSPDFR loads callout
  plugins itself. `scripts\` is *not* RPH's folder.
- **ScriptHookV auto-loads every `.asi` in the game root** (`dinput8.dll` runs before RPH
  initializes). A launch that bypasses the RPH launcher therefore *still* starts
  `SentinelUI.asi` — a **headless host**: hooks + helper run, but without Bootstrap there is no
  `Sui.Initialize`, no event pump and no graceful `SuiShutdown`. Observed 2026-09-10: the game
  wedged in termination (zombie process; helpers stayed alive because the pipe never dropped,
  and no `Shutdown requested` was ever logged). Test harnesses must launch via
  `RagePluginHook.exe` and treat a missing `[SentinelUI] Bootstrap ready` as a failed session.
- **Root ASI loading is also load-order-critical — RPH's Present-hook guard reverts foreign
  hooks installed after its own.** Tried the "clean" alternative on 2026-09-11: move
  `SentinelUI.asi` into `<game>\SentinelUI\` and have Bootstrap `LoadLibrary` it at plugin
  load. Instant crash: RPH detected the hook we installed after it
  (`Hook on our Present hook detected / Restoring untouched D3D11Present`) and rewrote the
  function; the D3D chain (GTA → Steam overlay → ShadowLib → `dxgi!Present`) then AV'd at the
  restored address. Our own minidump + RPH `crashreport` confirmed it. Conclusion: the ASI
  must be root-loaded by ScriptHookV *before* RPH arms its guard, and Bootstrap may only
  attach to the already-loaded module. A subfolder layout requires rendering through RPH
  (`Game.RawFrameRender`/`Graphics`) instead of a Present detour.
- **GTA V's own UI ignores synthetic input.** SendInput keyboard/mouse, posted `WM_KEY*`,
  `WM_ACTIVATE(APP)`, `WM_SYSCOMMAND/SC_CLOSE` and `WM_QUERYENDSESSION`/`WM_ENDSESSION` were
  all ignored by the game's quit prompt (DirectInput/RawInput input path), while the
  SentinelUI F9 hotkey still worked because the ASI reads WndProc messages directly.
  Consequence for tooling: unattended harnesses can drive SentinelUI itself, but cannot
  click "Ja" on the quit prompt or start story mode from the landing page — plan a manual
  step, or accept the kill/orphan-guard path for shutdown-lifecycle verification.
- **Log discipline**: LSPDFR/RPH logging is extremely verbose (1.2 MB of `[TRACE]` noise from a
  session with a couple of plugins). A UI runtime must write its **own logs** and emit only a
  handful of lifecycle lines into RagePluginHook.log.
- .NET Framework 4.8 is the effective floor (RPH SDK 1.124 targets net48). x64 only.
- Single-player modding is fine; RPH disables GTA Online itself.

## 3. CEF / Chromium embedding rules (the expensive ones)

- **CEF (post-M128 "bootstrap-era", all 2025+ builds incl. LTS 144) CANNOT be embedded from a
  DLL inside a foreign process.** Symptom: `0xC0000409` failfast at the **very first libcef call**
  (our bisect: crash inside `CefString(...)` assignment — the delay-load contact point), no SEH,
  no WER report, no stack. The **identical code from an EXE process works perfectly**.
- **Consequence: run CEF in its own EXE process** (helper process), share frames via named shared
  memory, and let a thin in-game module be a pure compositor. Bonus: page crashes can't kill the
  game, and CEF lifecycle/teardown problems disappear.
- **`CefResponse::SetMimeType` wants a BARE MIME type.** `text/html; charset=utf-8` yields an
  unparseable Content-Type and the renderer displays the page as a **source view** (escaped
  HTML inside a `<pre>`) instead of parsing it — subresources silently never load. Serve
  `text/html`, put `<meta charset>` in the page. Cost ~an hour of bisecting to find (2026-09-09).
- Custom standard schemes must be registered in **every process** (`OnRegisterCustomSchemes`
  fires per process — pass the app into `CefExecuteProcess` for children). The scheme's
  relative subresource resolution and HTML parsing only happen when the renderer agrees it
  is standard.
- `libcef.dll` resolution for a delay-loading module: the wrapper's scoped loader relocates
  `libcef.dll` **relative to the module/exe directory** — colocate the whole CEF runtime flat
  next to the host; set explicit `resources_dir_path`/`locales_dir_path` anyway.
- A **separate worker/subprocess executable** is not required if your process is already an EXE
  (self-spawn via `CefExecuteProcess` in `wmain`/`wWinMain`). It IS required when the host is a
  DLL — so: don't host CEF in a DLL.
- Windows OSR is effectively **always transparent** these days (`SetAsWindowless(hwnd)` only);
  background_color in browser settings controls composite alpha. `--disable-gpu` gives a stable
  software OSR path.
- A static page paints once or twice then **goes fully idle** — the hidden/static overlay cost is
  genuinely ~zero. (Verified: 2–3 paints in 45 s on a static page.)
- **`SendMouseClickEvent`'s third parameter is `mouseUp` (not mouseDown).** Passing a down flag
  through directly inverts every click (CEF sees up-before-down); buttons then fire only on every
  second click. Symptom: "buttons are buggy, sometimes they work". Fixed 2026-09-11. Also
  synthesize button-ups when capture ends (focus loss/F10) — a stuck held button blocks later clicks.
- **Custom schemes are scriptable but not fetchable.** `sentinelui://` pages can load scripts,
  styles, images and iframes, but `fetch()` and XHR to the scheme fail with
  `URL scheme "sentinelui" is not supported` (Chromium restriction, not CSP). For SPA-style
  routers either bundle screen fragments into a JS file loaded with `<script src>` or keep all
  screens in one document. Related: the scheme CSP needed `connect-src 'self'` (was `'none'`)
  before any fetch was even attempted; a blocked fetch shows a CSP console error, an unsupported
  scheme shows the URL scheme error.
- CEF's own logs (`debug.log`/`chrome_debug.log`) and console dispatch tell you a lot — enable
  them while diagnosing, disable in release.

## 4. Deployment pitfalls

- ASI/tool placements, path quoting, and **CEF runtime colocation** account for most first-run
  failures in-game. Verify by **byte-identical hash** that staged DLLs are intact (a corrupted
  copy is the first suspect when an EXE works but the game doesn't).
- Keep content/logs/cache in a **subfolder resolved by absolute path** (loader-independent) while
  the modules themselves live at root.
- Release hygiene: ship the CEF BSD license/attribution; document a clean-revert list.

## 5. Tooling / diagnostics (Windows)

- **WER**: `0xC0000409` failfast from a console app may produce **no Event Log entry and no
  report** — don't rely on WER for fastfails. For real crashes, WER LocalDumps needs **HKLM
  (admin)**; HKCU-scope didn't produce dumps for us.
- The **WindowsApps `cdbX64.exe` shim is dead** (produces nothing). Don't rely on it.
- **MinGW `gdb` cannot parse modern .NET/mixed PEs** ("not in executable format").
- **Ghidra 12 requires JDK 21 and dropped built-in Jython** (needs PyGhidra); **Ghidra 10.4 +
  JDK 17** still bundles Jython and works headless out of the box with classic GhidraScript API.
  Headless gotcha: the project directory must already exist; `!` in paths breaks the `.bat`
  wrapper (delayed expansion) — copy files to a clean path first.
- `dumpbin /imports` (VS toolset) is the fastest way to see how a binary links (static vs
  **delay-load** imports — WebUI.asi delay-loads `libcef.dll`, exactly like we did).

## 6. Method lessons

- **Instrument before theorizing**: a raw Win32 tracefile (CreateFileW/WriteFile, no CRT) with
  per-statement markers turned an opaque failfast into a 3-line bisect.
- Every copied binary preserves provenance: keep SHA-256 from build to deploy.
- Single-subject adversarial advice (external critique) caught our weak spots before shipping —
  e.g., AppDomain resolution, hook-order stacking, CEF version churn.
- **The human player test catches bugs machines never will** — the spaced-path bug and the
  top-left scaling issue were both found by the operator in real gameplay, not by any harness.